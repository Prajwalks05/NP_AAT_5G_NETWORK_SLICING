/*
 * 5G Network Slicing with Dynamic Traffic using NS-3 + 5G-LENA (NR module)
 *
 * Target versions : ns-3.38 + 5G-LENA (nr) v2.4   (see README, run scripts/check_api.sh first)
 * Topology        : RemoteHost -- PGW/EPC -- gNB -- {eMBB UEs, URLLC UEs, mMTC UEs}
 * Direction       : downlink (remote host -> UEs), UDP
 *
 * Slice resource control (IMPORTANT, see README "Limitations"):
 *   5G-LENA has no runtime per-slice PRB-quota API. The closest technically valid
 *   mechanism implemented here is a slice-level token-bucket POLICER at the core
 *   ingress (remote host side). Slice i may inject at most
 *        share_i(t) * cellCapacityMbps
 *   into the RAN. share_i(t) is constant for static slicing and is updated by a
 *   rule-based controller once per window for dynamic slicing. Packets rejected by
 *   the policer are counted as offered-but-lost (they are NOT hidden).
 *   Every slice also has its own dedicated EPS bearer (different QCI) and UDP port.
 *
 * Metrics are computed from FlowMonitor counters (rxPackets, rxBytes, delaySum,
 * jitterSum) plus source-side offered-packet counters (needed because the policer
 * drops packets before FlowMonitor can see them).
 */

#include "ns3/antenna-module.h"
#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-module.h"
#include "ns3/internet-module.h"
#include "ns3/lte-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/nr-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("DynamicSlicing");

// ============================================================================
// Slice representation
// ============================================================================
enum class SliceType
{
    EMBB = 0,
    URLLC = 1,
    MMTC = 2
};
static const int NS_SLICES = 3;
static const char* SLICE_NAME[NS_SLICES] = {"eMBB", "URLLC", "mMTC"};

struct SliceInfo
{
    SliceType type;
    std::string name;
    double allocatedResource;     // percent of cell capacity currently granted
    double targetThroughput;      // Mbps (= current demand, updated every window)
    double latencyRequirement;    // ms
    double packetLossRequirement; // percent
    double measuredThroughput;    // Mbps (last window)
    double measuredLatency;       // ms   (last window, NaN if nothing received)
    double measuredPacketLoss;    // percent (last window)
};

struct Phase
{
    double start;              // seconds relative to traffic start
    double rateMbps[NS_SLICES]; // aggregate offered rate per slice
};

// ============================================================================
// Configuration (all overridable on the command line)
// ============================================================================
static uint32_t numEmbbUes = 2;
static uint32_t numUrllcUes = 2;
static uint32_t numMmtcUes = 10;
static double simulationTime = 50.0; // seconds of traffic timeline
static double trafficStart = 0.5;    // s, leaves time for attach
static double windowSec = 1.0;       // measurement / controller period
static bool enableDynamicSlicing = false;
static uint32_t seed = 1;
static std::string outDir = "results";
static std::string configFile = "";

// radio
static double bandwidthMhz = 10.0;
static double centralFreqGhz = 3.5;
static uint32_t numerology = 1;
static double gnbTxPowerDbm = 30.0;
static double coreDelayMs = 1.0;
// Cell capacity used for slice quotas. CALIBRATE (README section "Calibration").
static double cellCapacityMbps = 50.0;

// slices
static uint16_t slicePort[NS_SLICES] = {5000, 6000, 7000};
static uint32_t slicePktSize[NS_SLICES] = {1400, 100, 100};
static double initShare[NS_SLICES] = {60.0, 30.0, 10.0};

// QoS thresholds
static double embbMinFrac = 0.90;     // eMBB satisfied if tput >= frac * offered
static double embbMaxLatMs = 100.0;
static double urllcMaxLatMs = 10.0;
static double urllcMaxLossPct = 1.0;
static double mmtcMaxLossPct = 5.0;

// dynamic controller
static double minShare[NS_SLICES] = {10.0, 10.0, 5.0};
static double headroom = 1.2;       // demand * headroom
static double maxStepPct = 10.0;    // max change of any share per window
static double urllcBoostPct = 5.0;  // extra URLLC share when its QoS is violated

static std::vector<Phase> g_phases = {{0.0, {10.0, 0.1, 0.05}},
                                      {10.0, {50.0, 0.1, 0.05}},
                                      {20.0, {50.0, 5.0, 0.05}},
                                      {30.0, {20.0, 5.0, 5.0}},
                                      {40.0, {80.0, 10.0, 10.0}}};

static uint32_t g_numUes[NS_SLICES];

// ============================================================================
// Global state
// ============================================================================
static SliceInfo g_slice[NS_SLICES];

struct Cum
{
    double tx = 0, rx = 0, rxBytes = 0, delay = 0, jitter = 0;
    double offPk = 0, offBytes = 0, policed = 0;
};
static Cum g_prev[NS_SLICES];
static Cum g_total[NS_SLICES];

struct Totals
{
    double shareSum = 0, utilSum = 0;
    uint32_t windows = 0, satisfied = 0;
    double offeredMbpsSum = 0;
};
static Totals g_tot[NS_SLICES];

static Ptr<FlowMonitor> g_monitor;
static Ptr<Ipv4FlowClassifier> g_classifier;
static std::ofstream g_tsFile;
static std::vector<Ptr<PacketSink>> g_sinks;
static std::vector<int> g_sinkSlice;

// source-side counters (offered load, before policing)
static double g_offPk[NS_SLICES], g_offBytes[NS_SLICES], g_policedPk[NS_SLICES];

// ============================================================================
// Slice policer (token bucket) -- the slice-level resource-control mechanism
// ============================================================================
struct SlicePolicer
{
    double rateBps = 0.0;
    double tokens = 0.0;
    double depth = 3000.0;
    Time last = Seconds(0);

    void SetRate(double bps)
    {
        Refill();
        rateBps = bps;
        depth = std::max(3000.0, rateBps / 8.0 * 0.02); // 20 ms burst, >= 2 packets
        tokens = std::min(tokens, depth);
    }
    void Refill()
    {
        Time now = Simulator::Now();
        tokens = std::min(depth, tokens + rateBps / 8.0 * (now - last).GetSeconds());
        last = now;
    }
    bool Allow(uint32_t bytes)
    {
        Refill();
        if (tokens >= bytes)
        {
            tokens -= bytes;
            return true;
        }
        return false;
    }
};
static SlicePolicer g_policer[NS_SLICES];

static void
ApplyPolicerRates()
{
    for (int s = 0; s < NS_SLICES; ++s)
    {
        g_policer[s].SetRate(g_slice[s].allocatedResource / 100.0 * cellCapacityMbps * 1e6);
    }
}

// ============================================================================
// Traffic phases
// ============================================================================
static double
SliceRateMbps(int s, double tRel)
{
    if (tRel < -1e-9)
    {
        return 0.0;
    }
    double r = 0.0;
    for (const auto& p : g_phases)
    {
        if (tRel + 1e-6 >= p.start)
        {
            r = p.rateMbps[s];
        }
    }
    return r;
}

static double
NextBoundary(double tRel)
{
    double b = std::numeric_limits<double>::infinity();
    for (const auto& p : g_phases)
    {
        if (p.start > tRel + 1e-6)
        {
            b = std::min(b, p.start);
        }
    }
    return b;
}

static int
SliceOfPort(uint16_t port)
{
    for (int s = 0; s < NS_SLICES; ++s)
    {
        if (slicePort[s] == port)
        {
            return s;
        }
    }
    return -1;
}

/**
 * UDP source with a time-varying rate (phases). Needed because OnOffApplication has
 * no supported runtime rate schedule. One instance per (slice, UE).
 */
class SliceSource : public Application
{
  public:
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::SliceSource")
                                .SetParent<Application>()
                                .SetGroupName("Applications")
                                .AddConstructor<SliceSource>();
        return tid;
    }
    SliceSource() = default;

    void Setup(int slice, Ipv4Address dst, uint16_t port, uint32_t pktSize, uint32_t nUes)
    {
        m_slice = slice;
        m_dst = dst;
        m_port = port;
        m_pktSize = pktSize;
        m_nUes = std::max<uint32_t>(1, nUes);
        m_rng = CreateObject<UniformRandomVariable>();
    }

  private:
    void StartApplication() override
    {
        m_socket = Socket::CreateSocket(GetNode(), UdpSocketFactory::GetTypeId());
        m_socket->Bind();
        m_socket->Connect(InetSocketAddress(m_dst, m_port));
        double r = PerUeRateBps(0.0);
        double iv = (r > 0) ? m_pktSize * 8.0 / r : 0.1;
        Time first = Seconds(m_rng->GetValue(0.0, std::min(iv, 0.1)));
        m_ev = Simulator::Schedule(first, &SliceSource::Tick, this);
    }
    void StopApplication() override
    {
        Simulator::Cancel(m_ev);
        if (m_socket)
        {
            m_socket->Close();
        }
    }
    double PerUeRateBps(double tRel) const
    {
        return SliceRateMbps(m_slice, tRel) * 1e6 / m_nUes;
    }
    void Tick()
    {
        double tRel = Simulator::Now().GetSeconds() - trafficStart;
        double r = PerUeRateBps(tRel);
        if (r > 0)
        {
            g_offPk[m_slice] += 1;
            g_offBytes[m_slice] += m_pktSize;
            if (g_policer[m_slice].Allow(m_pktSize))
            {
                m_socket->Send(Create<Packet>(m_pktSize));
            }
            else
            {
                g_policedPk[m_slice] += 1;
            }
        }
        double dt = (r > 0) ? m_pktSize * 8.0 / r : std::numeric_limits<double>::infinity();
        double toBoundary = NextBoundary(tRel) - tRel;
        if (toBoundary < dt)
        {
            dt = toBoundary; // re-evaluate exactly at the phase boundary
        }
        if (std::isinf(dt))
        {
            return; // no more traffic in this slice
        }
        m_ev = Simulator::Schedule(Seconds(dt), &SliceSource::Tick, this);
    }

    int m_slice = 0;
    Ipv4Address m_dst;
    uint16_t m_port = 0;
    uint32_t m_pktSize = 100;
    uint32_t m_nUes = 1;
    Ptr<Socket> m_socket;
    EventId m_ev;
    Ptr<UniformRandomVariable> m_rng;
};
NS_OBJECT_ENSURE_REGISTERED(SliceSource);

// ============================================================================
// Helpers
// ============================================================================
static std::string
Num(double v, int prec = 6)
{
    if (std::isnan(v) || std::isinf(v))
    {
        return "";
    }
    std::ostringstream o;
    o << std::fixed << std::setprecision(prec) << v;
    return o.str();
}

static bool
FileHasContent(const std::string& path)
{
    std::ifstream f(path);
    return f.good() && f.peek() != std::ifstream::traits_type::eof();
}

static void
LoadConfigFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in.good())
    {
        NS_FATAL_ERROR("Cannot open --configFile=" << path);
    }
    std::vector<Phase> phases;
    std::string line;
    while (std::getline(in, line))
    {
        auto h = line.find('#');
        if (h != std::string::npos)
        {
            line = line.substr(0, h);
        }
        auto eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
        if (key == "phase")
        {
            std::replace(val.begin(), val.end(), ',', ' ');
            std::istringstream is(val);
            Phase p;
            if (is >> p.start >> p.rateMbps[0] >> p.rateMbps[1] >> p.rateMbps[2])
            {
                phases.push_back(p);
            }
            else
            {
                NS_FATAL_ERROR("Bad phase line in config: " << line);
            }
        }
    }
    if (!phases.empty())
    {
        std::sort(phases.begin(), phases.end(), [](const Phase& a, const Phase& b) {
            return a.start < b.start;
        });
        g_phases = phases;
    }
}

static int
FlowSlice(FlowId id)
{
    Ipv4FlowClassifier::FiveTuple t = g_classifier->FindFlow(id);
    return SliceOfPort(t.destinationPort);
}

static void
GatherCum(Cum cur[NS_SLICES])
{
    for (int s = 0; s < NS_SLICES; ++s)
    {
        cur[s] = Cum();
        cur[s].offPk = g_offPk[s];
        cur[s].offBytes = g_offBytes[s];
        cur[s].policed = g_policedPk[s];
    }
    for (const auto& kv : g_monitor->GetFlowStats())
    {
        int s = FlowSlice(kv.first);
        if (s < 0)
        {
            continue; // not one of our slice flows
        }
        cur[s].tx += kv.second.txPackets;
        cur[s].rx += kv.second.rxPackets;
        cur[s].rxBytes += kv.second.rxBytes;
        cur[s].delay += kv.second.delaySum.GetSeconds();
        cur[s].jitter += kv.second.jitterSum.GetSeconds();
    }
}

static bool
EvalQos(int s, double offered, bool gotRx, double tput, double latMs, double lossPct)
{
    if (offered <= 0.0)
    {
        return true; // no demand -> nothing to violate
    }
    switch (s)
    {
    case 0: // eMBB
        return gotRx && tput >= embbMinFrac * offered && latMs <= embbMaxLatMs;
    case 1: // URLLC
        return gotRx && latMs <= urllcMaxLatMs && lossPct <= urllcMaxLossPct;
    default: // mMTC: connectivity / delivery ratio
        return gotRx && lossPct <= mmtcMaxLossPct;
    }
}

// ============================================================================
// Dynamic controller (rule based, deterministic)
// ============================================================================
static void
UpdateDynamic(const double offered[], const double latMs[], const bool gotRx[], const double lossPct[])
{
    double cap = cellCapacityMbps;
    double target[NS_SLICES];
    for (int s = 0; s < NS_SLICES; ++s)
    {
        double demandShare = offered[s] * headroom / cap * 100.0;
        target[s] = std::max(minShare[s], demandShare);
    }
    // Rule 1: URLLC QoS violated -> boost URLLC target
    bool urllcViolated = offered[1] > 0 && (!gotRx[1] || latMs[1] > urllcMaxLatMs || lossPct[1] > urllcMaxLossPct);
    if (urllcViolated)
    {
        target[1] += urllcBoostPct;
    }

    double sum = target[0] + target[1] + target[2];
    double nw[NS_SLICES];
    if (sum <= 100.0)
    {
        // Rule 2: spare capacity distributed proportionally to demand
        double spare = 100.0 - sum;
        for (int s = 0; s < NS_SLICES; ++s)
        {
            nw[s] = target[s] + spare * target[s] / sum;
        }
    }
    else
    {
        // Rule 3: overload -> priority URLLC > mMTC > eMBB, lower slices keep their floor
        const int order[NS_SLICES] = {1, 2, 0};
        double remaining = 100.0;
        for (int k = 0; k < NS_SLICES; ++k)
        {
            int s = order[k];
            if (k == NS_SLICES - 1)
            {
                nw[s] = remaining;
                break;
            }
            double reserve = 0.0;
            for (int j = k + 1; j < NS_SLICES; ++j)
            {
                reserve += minShare[order[j]];
            }
            nw[s] = std::min(target[s], std::max(minShare[s], remaining - reserve));
            remaining -= nw[s];
        }
    }
    // Rate limit: scale all deltas so the largest move is <= maxStepPct (sum stays 100)
    double maxDelta = 0.0;
    for (int s = 0; s < NS_SLICES; ++s)
    {
        maxDelta = std::max(maxDelta, std::fabs(nw[s] - g_slice[s].allocatedResource));
    }
    double f = (maxDelta > maxStepPct) ? maxStepPct / maxDelta : 1.0;
    for (int s = 0; s < NS_SLICES; ++s)
    {
        g_slice[s].allocatedResource += f * (nw[s] - g_slice[s].allocatedResource);
    }
    ApplyPolicerRates();
}

// ============================================================================
// Periodic measurement
// ============================================================================
static void
WindowEnd(uint32_t k)
{
    Cum cur[NS_SLICES];
    GatherCum(cur);
    double tStart = k * windowSec;
    double offered[NS_SLICES], latMs[NS_SLICES], lossPct[NS_SLICES];
    bool gotRx[NS_SLICES];

    for (int s = 0; s < NS_SLICES; ++s)
    {
        Cum d;
        d.tx = cur[s].tx - g_prev[s].tx;
        d.rx = cur[s].rx - g_prev[s].rx;
        d.rxBytes = cur[s].rxBytes - g_prev[s].rxBytes;
        d.delay = cur[s].delay - g_prev[s].delay;
        d.jitter = cur[s].jitter - g_prev[s].jitter;
        d.offPk = cur[s].offPk - g_prev[s].offPk;
        d.offBytes = cur[s].offBytes - g_prev[s].offBytes;
        d.policed = cur[s].policed - g_prev[s].policed;

        offered[s] = d.offBytes * 8.0 / 1e6 / windowSec;
        double tput = d.rxBytes * 8.0 / 1e6 / windowSec;
        gotRx[s] = d.rx > 0;
        latMs[s] = gotRx[s] ? d.delay / d.rx * 1000.0 : std::numeric_limits<double>::quiet_NaN();
        double jitMs = gotRx[s] ? d.jitter / d.rx * 1000.0 : std::numeric_limits<double>::quiet_NaN();
        lossPct[s] = d.offPk > 0 ? std::max(0.0, (d.offPk - d.rx) / d.offPk * 100.0) : 0.0;
        double share = g_slice[s].allocatedResource;
        double quota = share / 100.0 * cellCapacityMbps;
        double util = quota > 0 ? tput / quota * 100.0 : 0.0;
        bool ok = EvalQos(s, offered[s], gotRx[s], tput, latMs[s], lossPct[s]);

        g_slice[s].targetThroughput = offered[s];
        g_slice[s].measuredThroughput = tput;
        g_slice[s].measuredLatency = latMs[s];
        g_slice[s].measuredPacketLoss = lossPct[s];

        // accumulate
        g_total[s].tx += d.tx;
        g_total[s].rx += d.rx;
        g_total[s].rxBytes += d.rxBytes;
        g_total[s].delay += d.delay;
        g_total[s].jitter += d.jitter;
        g_total[s].offPk += d.offPk;
        g_total[s].offBytes += d.offBytes;
        g_total[s].policed += d.policed;
        g_tot[s].windows++;
        g_tot[s].satisfied += ok ? 1 : 0;
        g_tot[s].shareSum += share;
        g_tot[s].utilSum += util;
        g_tot[s].offeredMbpsSum += offered[s];

        g_tsFile << (enableDynamicSlicing ? "dynamic" : "static") << "," << seed << ","
                 << Num(tStart, 3) << "," << SLICE_NAME[s] << "," << Num(offered[s]) << ","
                 << Num(tput) << "," << Num(latMs[s]) << "," << Num(lossPct[s]) << ","
                 << Num(jitMs) << "," << Num(share, 3) << "," << Num(util, 3) << ","
                 << (ok ? 1 : 0) << "\n";

        std::cout << "[t=" << std::setw(5) << tStart << "s] " << std::setw(5) << SLICE_NAME[s]
                  << " offered=" << std::setw(8) << Num(offered[s], 3)
                  << " Mbps  tput=" << std::setw(8) << Num(tput, 3)
                  << " Mbps  lat=" << std::setw(8) << Num(latMs[s], 2)
                  << " ms  loss=" << std::setw(7) << Num(lossPct[s], 2)
                  << " %  share=" << std::setw(6) << Num(share, 1) << " %  qos=" << ok << "\n";
    }
    for (int s = 0; s < NS_SLICES; ++s)
    {
        g_prev[s] = cur[s];
    }
    if (enableDynamicSlicing)
    {
        UpdateDynamic(offered, latMs, gotRx, lossPct);
    }
}

// ============================================================================
// main
// ============================================================================
int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);
    cmd.AddValue("numEmbbUes", "Number of eMBB UEs", numEmbbUes);
    cmd.AddValue("numUrllcUes", "Number of URLLC UEs", numUrllcUes);
    cmd.AddValue("numMmtcUes", "Number of mMTC UEs", numMmtcUes);
    cmd.AddValue("simTime", "Traffic timeline length [s]", simulationTime);
    cmd.AddValue("trafficStart", "Traffic start time [s]", trafficStart);
    cmd.AddValue("window", "Measurement/controller period [s]", windowSec);
    cmd.AddValue("enableDynamicSlicing", "0=static, 1=dynamic", enableDynamicSlicing);
    cmd.AddValue("seed", "RNG seed", seed);
    cmd.AddValue("outDir", "Output directory for CSV files", outDir);
    cmd.AddValue("configFile", "Optional file with 'phase = start,embb,urllc,mmtc' lines", configFile);
    cmd.AddValue("bandwidthMhz", "NR carrier bandwidth [MHz]", bandwidthMhz);
    cmd.AddValue("centralFreqGhz", "Carrier frequency [GHz]", centralFreqGhz);
    cmd.AddValue("numerology", "NR numerology", numerology);
    cmd.AddValue("gnbTxPowerDbm", "gNB TX power [dBm]", gnbTxPowerDbm);
    cmd.AddValue("coreDelayMs", "RemoteHost-PGW link delay [ms]", coreDelayMs);
    cmd.AddValue("cellCapacityMbps", "Capacity used for slice quotas [Mbps]", cellCapacityMbps);
    cmd.AddValue("portEmbb", "UDP port eMBB", slicePort[0]);
    cmd.AddValue("portUrllc", "UDP port URLLC", slicePort[1]);
    cmd.AddValue("portMmtc", "UDP port mMTC", slicePort[2]);
    cmd.AddValue("pktEmbb", "eMBB packet size [B]", slicePktSize[0]);
    cmd.AddValue("pktUrllc", "URLLC packet size [B]", slicePktSize[1]);
    cmd.AddValue("pktMmtc", "mMTC packet size [B]", slicePktSize[2]);
    cmd.AddValue("shareEmbb", "Initial eMBB share [%]", initShare[0]);
    cmd.AddValue("shareUrllc", "Initial URLLC share [%]", initShare[1]);
    cmd.AddValue("shareMmtc", "Initial mMTC share [%]", initShare[2]);
    cmd.AddValue("urllcMaxLatMs", "URLLC latency threshold [ms]", urllcMaxLatMs);
    cmd.AddValue("urllcMaxLossPct", "URLLC loss threshold [%]", urllcMaxLossPct);
    cmd.AddValue("embbMinFrac", "eMBB: required tput/offered ratio", embbMinFrac);
    cmd.AddValue("embbMaxLatMs", "eMBB latency threshold [ms]", embbMaxLatMs);
    cmd.AddValue("mmtcMaxLossPct", "mMTC loss threshold [%]", mmtcMaxLossPct);
    cmd.AddValue("minShareEmbb", "Dynamic: eMBB floor [%]", minShare[0]);
    cmd.AddValue("minShareUrllc", "Dynamic: URLLC floor [%]", minShare[1]);
    cmd.AddValue("minShareMmtc", "Dynamic: mMTC floor [%]", minShare[2]);
    cmd.AddValue("headroom", "Dynamic: demand multiplier", headroom);
    cmd.AddValue("maxStepPct", "Dynamic: max share change per window [%]", maxStepPct);
    cmd.AddValue("urllcBoostPct", "Dynamic: URLLC boost on violation [%]", urllcBoostPct);
    cmd.Parse(argc, argv);

    if (!configFile.empty())
    {
        LoadConfigFile(configFile);
    }
    g_numUes[0] = numEmbbUes;
    g_numUes[1] = numUrllcUes;
    g_numUes[2] = numMmtcUes;
    uint32_t totalUes = numEmbbUes + numUrllcUes + numMmtcUes;
    NS_ABORT_MSG_IF(totalUes == 0, "need at least one UE");
    double shareTotal = initShare[0] + initShare[1] + initShare[2];
    NS_ABORT_MSG_IF(std::fabs(shareTotal - 100.0) > 1e-6, "initial shares must sum to 100");

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(1);

    const double simEnd = trafficStart + simulationTime + 1.0;

    // slice descriptors
    const double lat[NS_SLICES] = {embbMaxLatMs, urllcMaxLatMs, 0.0};
    const double loss[NS_SLICES] = {100.0 * (1.0 - embbMinFrac), urllcMaxLossPct, mmtcMaxLossPct};
    for (int s = 0; s < NS_SLICES; ++s)
    {
        g_slice[s] = SliceInfo{static_cast<SliceType>(s), SLICE_NAME[s], initShare[s], 0.0,
                               lat[s], loss[s], 0.0, 0.0, 0.0};
    }
    ApplyPolicerRates();

    // ---------------- NR / EPC ----------------
    Ptr<NrPointToPointEpcHelper> epcHelper = CreateObject<NrPointToPointEpcHelper>();
    Ptr<IdealBeamformingHelper> bfHelper = CreateObject<IdealBeamformingHelper>();
    Ptr<NrHelper> nrHelper = CreateObject<NrHelper>();
    nrHelper->SetBeamformingHelper(bfHelper);
    nrHelper->SetEpcHelper(epcHelper);
    bfHelper->SetAttribute("BeamformingMethod", TypeIdValue(DirectPathBeamforming::GetTypeId()));
    epcHelper->SetAttribute("S1uLinkDelay", TimeValue(MilliSeconds(0)));

    NodeContainer gnbNodes;
    gnbNodes.Create(1);
    NodeContainer ueNodes;
    ueNodes.Create(totalUes);

    MobilityHelper mob;
    mob.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    Ptr<ListPositionAllocator> gp = CreateObject<ListPositionAllocator>();
    gp->Add(Vector(0.0, 0.0, 10.0));
    mob.SetPositionAllocator(gp);
    mob.Install(gnbNodes);
    Ptr<ListPositionAllocator> up = CreateObject<ListPositionAllocator>();
    for (uint32_t i = 0; i < totalUes; ++i)
    {
        double ang = 2.0 * M_PI * i / totalUes;
        double rad = 30.0 + 10.0 * (i % 3);
        up->Add(Vector(rad * std::cos(ang), rad * std::sin(ang), 1.5));
    }
    mob.SetPositionAllocator(up);
    mob.Install(ueNodes);

    CcBwpCreator ccBwpCreator;
    CcBwpCreator::SimpleOperationBandConf bandConf(centralFreqGhz * 1e9,
                                                   bandwidthMhz * 1e6,
                                                   1,
                                                   BandwidthPartInfo::UMi_StreetCanyon);
    OperationBandInfo band = ccBwpCreator.CreateOperationBandContiguousCc(bandConf);
    nrHelper->SetChannelConditionModelAttribute("UpdatePeriod", TimeValue(MilliSeconds(0)));
    nrHelper->SetPathlossAttribute("ShadowingEnabled", BooleanValue(false));
    nrHelper->InitializeOperationBand(&band);
    BandwidthPartInfoPtrVector allBwps = CcBwpCreator::GetAllBwps({band});

    nrHelper->SetSchedulerTypeId(NrMacSchedulerTdmaRR::GetTypeId());
    nrHelper->SetUeAntennaAttribute("NumRows", UintegerValue(2));
    nrHelper->SetUeAntennaAttribute("NumColumns", UintegerValue(2));
    nrHelper->SetUeAntennaAttribute("AntennaElement",
                                    PointerValue(CreateObject<IsotropicAntennaModel>()));
    nrHelper->SetGnbAntennaAttribute("NumRows", UintegerValue(4));
    nrHelper->SetGnbAntennaAttribute("NumColumns", UintegerValue(4));
    nrHelper->SetGnbAntennaAttribute("AntennaElement",
                                     PointerValue(CreateObject<IsotropicAntennaModel>()));

    NetDeviceContainer gnbDevs = nrHelper->InstallGnbDevice(gnbNodes, allBwps);
    NetDeviceContainer ueDevs = nrHelper->InstallUeDevice(ueNodes, allBwps);
    int64_t stream = 1;
    stream += nrHelper->AssignStreams(gnbDevs, stream);
    stream += nrHelper->AssignStreams(ueDevs, stream);

    nrHelper->GetGnbPhy(gnbDevs.Get(0), 0)->SetAttribute("Numerology", UintegerValue(numerology));
    nrHelper->GetGnbPhy(gnbDevs.Get(0), 0)->SetAttribute("TxPower", DoubleValue(gnbTxPowerDbm));
    for (auto it = gnbDevs.Begin(); it != gnbDevs.End(); ++it)
    {
        DynamicCast<NrGnbNetDevice>(*it)->UpdateConfig();
    }
    for (auto it = ueDevs.Begin(); it != ueDevs.End(); ++it)
    {
        DynamicCast<NrUeNetDevice>(*it)->UpdateConfig();
    }

    // ---------------- Remote host + IP ----------------
    Ptr<Node> pgw = epcHelper->GetPgwNode();
    NodeContainer remoteHostContainer;
    remoteHostContainer.Create(1);
    Ptr<Node> remoteHost = remoteHostContainer.Get(0);
    InternetStackHelper internet;
    internet.Install(remoteHostContainer);

    PointToPointHelper p2p;
    p2p.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Gb/s")));
    p2p.SetDeviceAttribute("Mtu", UintegerValue(2500));
    p2p.SetChannelAttribute("Delay", TimeValue(MilliSeconds(coreDelayMs)));
    NetDeviceContainer inetDevs = p2p.Install(pgw, remoteHost);
    Ipv4AddressHelper ipv4h;
    ipv4h.SetBase("1.0.0.0", "255.0.0.0");
    ipv4h.Assign(inetDevs);
    Ipv4StaticRoutingHelper routingHelper;
    Ptr<Ipv4StaticRouting> rhRouting =
        routingHelper.GetStaticRouting(remoteHost->GetObject<Ipv4>());
    rhRouting->AddNetworkRouteTo(Ipv4Address("7.0.0.0"), Ipv4Mask("255.0.0.0"), 1);

    internet.Install(ueNodes);
    Ipv4InterfaceContainer ueIfaces = epcHelper->AssignUeIpv4Address(NetDeviceContainer(ueDevs));
    for (uint32_t j = 0; j < ueNodes.GetN(); ++j)
    {
        Ptr<Ipv4StaticRouting> r = routingHelper.GetStaticRouting(ueNodes.Get(j)->GetObject<Ipv4>());
        r->SetDefaultRoute(epcHelper->GetUeDefaultGatewayAddress(), 1);
    }
    nrHelper->AttachToClosestEnb(ueDevs, gnbDevs);

    // ---------------- Slice <-> UE mapping, bearers ----------------
    uint32_t first[NS_SLICES] = {0, numEmbbUes, numEmbbUes + numUrllcUes};
    const EpsBearer::Qci qci[NS_SLICES] = {EpsBearer::NGBR_VIDEO_TCP_PREMIUM,
                                           EpsBearer::NGBR_LOW_LAT_EMBB,
                                           EpsBearer::NGBR_VIDEO_TCP_DEFAULT};
    for (int s = 0; s < NS_SLICES; ++s)
    {
        if (g_numUes[s] == 0)
        {
            continue;
        }
        NetDeviceContainer devs;
        for (uint32_t i = 0; i < g_numUes[s]; ++i)
        {
            devs.Add(ueDevs.Get(first[s] + i));
        }
        Ptr<EpcTft> tft = Create<EpcTft>();
        EpcTft::PacketFilter pf;
        pf.localPortStart = slicePort[s];
        pf.localPortEnd = slicePort[s];
        tft->Add(pf);
        nrHelper->ActivateDedicatedEpsBearer(devs, EpsBearer(qci[s]), tft);
    }

    // ---------------- Applications ----------------
    for (int s = 0; s < NS_SLICES; ++s)
    {
        for (uint32_t i = 0; i < g_numUes[s]; ++i)
        {
            uint32_t idx = first[s] + i;
            Ptr<Node> ue = ueNodes.Get(idx);
            PacketSinkHelper sinkH("ns3::UdpSocketFactory",
                                   InetSocketAddress(Ipv4Address::GetAny(), slicePort[s]));
            ApplicationContainer sc = sinkH.Install(ue);
            sc.Start(Seconds(0.0));
            sc.Stop(Seconds(simEnd));
            g_sinks.push_back(DynamicCast<PacketSink>(sc.Get(0)));
            g_sinkSlice.push_back(s);

            Ptr<SliceSource> src = CreateObject<SliceSource>();
            src->Setup(s, ueIfaces.GetAddress(idx), slicePort[s], slicePktSize[s], g_numUes[s]);
            remoteHost->AddApplication(src);
            src->SetStartTime(Seconds(trafficStart));
            src->SetStopTime(Seconds(simEnd));
            std::cout << "UE " << idx << " ip=" << ueIfaces.GetAddress(idx) << " slice="
                      << SLICE_NAME[s] << " port=" << slicePort[s] << "\n";
        }
    }

    // ---------------- FlowMonitor ----------------
    FlowMonitorHelper fmHelper;
    g_monitor = fmHelper.Install(remoteHost);
    fmHelper.Install(ueNodes);
    g_classifier = DynamicCast<Ipv4FlowClassifier>(fmHelper.GetClassifier());

    // ---------------- Output files ----------------
    std::filesystem::create_directories(outDir);
    const std::string tsPath = outDir + "/time_series.csv";
    bool tsNew = !FileHasContent(tsPath);
    g_tsFile.open(tsPath, std::ios::app);
    if (tsNew)
    {
        g_tsFile << "mode,seed,time,slice,offered_mbps,throughput_mbps,latency_ms,"
                    "packet_loss_percent,jitter_ms,resource_percent,utilization_percent,"
                    "qos_satisfied\n";
    }

    // ---------------- Schedule controller/measurement ----------------
    uint32_t nWindows = static_cast<uint32_t>(std::floor(simulationTime / windowSec + 1e-9));
    for (uint32_t k = 0; k < nWindows; ++k)
    {
        Simulator::Schedule(Seconds(trafficStart + (k + 1) * windowSec), &WindowEnd, k);
    }

    std::cout << "mode=" << (enableDynamicSlicing ? "dynamic" : "static") << " seed=" << seed
              << " simTime=" << simulationTime << " capacity=" << cellCapacityMbps << " Mbps\n";
    Simulator::Stop(Seconds(simEnd));
    Simulator::Run();

    // ---------------- Summary ----------------
    const std::string mode = enableDynamicSlicing ? "dynamic" : "static";
    const std::string sumPath = outDir + "/" + mode + "_results.csv";
    bool sNew = !FileHasContent(sumPath);
    std::ofstream sf(sumPath, std::ios::app);
    if (sNew)
    {
        sf << "mode,seed,slice,avg_throughput_mbps,avg_offered_mbps,avg_latency_ms,"
              "packet_loss_percent,jitter_ms,tx_packets,rx_packets,offered_packets,"
              "policed_packets,avg_resource_percent,avg_utilization_percent,"
              "qos_satisfaction_ratio\n";
    }
    int rc = 0;
    double dur = nWindows * windowSec;
    std::cout << "\n===== SUMMARY (" << mode << ", seed " << seed << ") =====\n";
    for (int s = 0; s < NS_SLICES; ++s)
    {
        const Cum& c = g_total[s];
        const Totals& t = g_tot[s];
        double tput = c.rxBytes * 8.0 / 1e6 / dur;
        double offM = t.windows ? t.offeredMbpsSum / t.windows : 0.0;
        double latMs = c.rx > 0 ? c.delay / c.rx * 1000.0 : std::numeric_limits<double>::quiet_NaN();
        double jitMs = c.rx > 0 ? c.jitter / c.rx * 1000.0 : std::numeric_limits<double>::quiet_NaN();
        double lossP = c.offPk > 0 ? std::max(0.0, (c.offPk - c.rx) / c.offPk * 100.0) : 0.0;
        double share = t.windows ? t.shareSum / t.windows : 0.0;
        double util = t.windows ? t.utilSum / t.windows : 0.0;
        double qos = t.windows ? static_cast<double>(t.satisfied) / t.windows : 0.0;
        sf << mode << "," << seed << "," << SLICE_NAME[s] << "," << Num(tput) << "," << Num(offM)
           << "," << Num(latMs) << "," << Num(lossP) << "," << Num(jitMs) << "," << c.tx << ","
           << c.rx << "," << c.offPk << "," << c.policed << "," << Num(share, 3) << ","
           << Num(util, 3) << "," << Num(qos, 4) << "\n";
        std::cout << SLICE_NAME[s] << ": tput=" << Num(tput, 3) << " Mbps lat=" << Num(latMs, 2)
                  << " ms loss=" << Num(lossP, 2) << " % jitter=" << Num(jitMs, 3)
                  << " ms offered=" << c.offPk << " pkts policed=" << c.policed
                  << " rx=" << c.rx << " avgShare=" << Num(share, 1) << " % qos=" << Num(qos, 3)
                  << "\n";
        if (g_numUes[s] > 0 && c.rx == 0)
        {
            std::cerr << "ERROR: slice " << SLICE_NAME[s] << " received no packets\n";
            rc = 2;
        }
    }
    std::cout << "\nPer-UE sink check (bytes received):\n";
    for (size_t i = 0; i < g_sinks.size(); ++i)
    {
        std::cout << "  sink " << i << " (" << SLICE_NAME[g_sinkSlice[i]]
                  << "): " << g_sinks[i]->GetTotalRx() << " B\n";
        if (g_sinks[i]->GetTotalRx() == 0)
        {
            std::cerr << "WARNING: sink " << i << " received nothing (attach/bearer problem?)\n";
        }
    }
    g_tsFile.close();
    Simulator::Destroy();
    return rc;
}
