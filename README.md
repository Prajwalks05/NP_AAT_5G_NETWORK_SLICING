# 5G Network Slicing with Dynamic Traffic using NS-3

> **Status: NOT yet compiled or run.** The code was written against the **ns-3.38 + 5G-LENA (nr) v2.4**
> API without access to those repositories. `results/` and `plots/` are empty on purpose: no numbers have
> been fabricated. Follow "Phase 0" below first; if a header check fails, adapt the listed call to the
> installed version (compare with `contrib/nr/examples/cttc-nr-demo.cc`).
> The Python analysis was exercised on synthetic data to test the code path only.

## Objective
Show quantitatively whether **dynamic** slice resource allocation improves QoS and resource
utilisation, compared with **static** allocation, when eMBB / URLLC / mMTC traffic changes over time.

## Technologies
NS-3 (3.38), 5G-LENA NR module (v2.4), C++17, FlowMonitor, Python 3, NumPy, Pandas, Matplotlib.

## Architecture
```
Remote Host --(p2p, 1 ms)-- PGW/EPC (NrPointToPointEpcHelper) -- gNB (1 carrier, 10 MHz @ 3.5 GHz, mu=1)
                                                                  /        |         \
                                                       eMBB UEs (2)  URLLC UEs (2)  mMTC UEs (10)
```
Downlink UDP, one dedicated EPS bearer per slice (different QCI, TFT on the slice UDP port),
ports eMBB 5000 / URLLC 6000 / mMTC 7000. Flow -> slice mapping is done from the FlowMonitor
classifier's destination port, so every metric is per slice.

## Traffic model (aggregate per slice, split equally among its UEs)
| Phase (s) | eMBB | URLLC | mMTC | Meaning |
|---|---|---|---|---|
| 0-10  | 10 Mbps | 0.1 Mbps | 0.05 Mbps | normal |
| 10-20 | 50 | 0.1 | 0.05 | eMBB spike |
| 20-30 | 50 | 5 | 0.05 | URLLC spike |
| 30-40 | 20 | 5 | 5 | mMTC spike |
| 40-50 | 80 | 10 | 10 | heavy mixed |

`OnOffApplication.DataRate` is not a supported run-time schedule, so a small custom `SliceSource`
application (in the .cc file) sends UDP packets at a rate looked up from the phase table and
re-evaluates exactly at each phase boundary. The table is editable in `config/simulation-config.txt`
(`--configFile=`). Traffic starts at `--trafficStart=0.5` s (attach time); reported times are relative to it.

## Slice resource control (what is real, and what is not)
5G-LENA has **no run-time per-slice PRB quota API**. The closest valid mechanism implemented here is a
**slice-level token-bucket policer at the core ingress**: slice *i* may inject at most
`share_i(t) x cellCapacityMbps` into the RAN. Policed packets are counted as offered-but-lost.
`resource_percent` is therefore the share of *cell capacity* granted by this policer,
**not a measured PRB count**. `utilization_percent = throughput / (share x capacity)`.
Inside the RAN all slices still share one carrier under `NrMacSchedulerTdmaRR`.

## Static slicing
Shares fixed at eMBB 60 / URLLC 30 / mMTC 10 % (`--shareEmbb/--shareUrllc/--shareMmtc`) for the whole run.

## Dynamic slicing (rule-based, deterministic, every 1 s)
1. `target_i = max(floor_i, offered_i * headroom / capacity)`; floors 10/10/5 %, headroom 1.2.
2. If URLLC latency > 10 ms, loss > 1 %, or nothing received: `target_URLLC += 5 %`.
3. If sum(targets) <= 100 %: spare capacity is split proportionally to the targets.
4. If sum > 100 % (overload): priority URLLC > mMTC > eMBB; lower-priority slices keep their floor.
5. Rate limit: no share moves more than 10 points per window (deltas scaled together, so the total stays 100 %).

## Metrics (per slice, per 1 s window and per run)
* **Throughput (Mbps)** = delta rxBytes x 8 / window.
* **Latency (ms)** = delta delaySum / delta rxPackets (FlowMonitor).
* **Packet loss (%)** = (offered - received) / offered, where *offered* is counted at the source
  before the policer (FlowMonitor's `lostPackets` is not used: it only updates after a long timeout
  and would not see policed packets). Packets in flight at window edges can cause a tiny error; negatives are clamped to 0.
* **Jitter (ms)** = delta jitterSum / delta rxPackets (approximation).
* **Resource allocation (%)** and **utilisation (%)** as defined above.
* **QoS satisfied (0/1)**: eMBB: throughput >= 0.9 x offered and latency <= 100 ms;
  URLLC: latency <= 10 ms and loss <= 1 %; mMTC: loss <= 5 % (delivery/connectivity). A window with no demand counts as satisfied.
  **Satisfaction ratio** = satisfied windows / windows. All thresholds are CLI options.
* Time column = **start** of the 1 s window.

## Phase 0: environment on Ubuntu 18.04
Ubuntu 18.04's default gcc 7 is too old for ns-3.38 (C++17) and its cmake is 3.10.
```bash
sudo apt update
sudo apt install -y software-properties-common git python3 python3-pip ninja-build ccache libsqlite3-dev libxml2-dev
sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test && sudo apt update
sudo apt install -y gcc-9 g++-9
pip3 install --user "cmake>=3.16" "numpy" "pandas<1.2" "matplotlib<3.4"   # last releases supporting Python 3.6
export PATH=$HOME/.local/bin:$PATH CC=gcc-9 CXX=g++-9

cd ~ && git clone https://gitlab.com/nsnam/ns-3-dev.git && cd ns-3-dev && git checkout ns-3.38
git clone https://gitlab.com/cttc-lena/nr.git contrib/nr
cd contrib/nr && git branch -r        # pick the branch for ns-3.38, e.g. 5g-lena-v2.4.y
git checkout 5g-lena-v2.4.y && cd ../..
```
(If you can, Ubuntu 22.04 avoids the PPA/pip workarounds; the project is otherwise identical.)

Check the installed versions and every API the code uses:
```bash
cd ~/5g-network-slicing && scripts/check_api.sh ~/ns-3-dev
```

## Build
```bash
cd ~/ns-3-dev
./ns3 configure -d optimized --enable-examples
cp ~/5g-network-slicing/scratch/dynamic-slicing.cc scratch/
./ns3 build
```
Smoke test before the full run (phases 1-2 of the development plan; one flow, one UE):
```bash
./ns3 run "dynamic-slicing --numEmbbUes=1 --numUrllcUes=0 --numMmtcUes=0 --simTime=5 --outDir=/tmp/smoke"
```
Read the log: every UE prints its IP and slice; each window prints offered/throughput/latency/loss/share;
the end prints a per-UE sink check. The program exits with code 2 if a slice with UEs received nothing.

## Run
```bash
P=~/5g-network-slicing
# static
./ns3 run "dynamic-slicing --enableDynamicSlicing=0 --seed=1 --outDir=$P/results --configFile=$P/config/simulation-config.txt"
# dynamic
./ns3 run "dynamic-slicing --enableDynamicSlicing=1 --seed=1 --outDir=$P/results --configFile=$P/config/simulation-config.txt"
# analysis
python3 $P/analysis/analyze_results.py
```
Or everything (3 seeds x 2 modes, deletes old CSVs first): `scripts/run_all.sh ~/ns-3-dev 1 2 3`.
Results are **appended**, so remove `results/*.csv` before repeating an experiment.

## Calibration (important)
`--cellCapacityMbps` (default 50) defines what 100 % means. Calibrate it before drawing conclusions:
run one eMBB UE with a very large load (config line `phase = 0, 200, 0, 0`) and `--shareEmbb=100 --shareUrllc=0 --shareMmtc=0 --cellCapacityMbps=1000`, then use the throughput actually reached as the capacity.
If capacity is overestimated the RAN, not the policer, becomes the bottleneck and latency/loss will be
dominated by the shared RLC queue; if underestimated the policer wastes capacity.
Phase 5 (100 Mbps offered) is designed to overload a ~50 Mbps cell.

## Output files
* `results/time_series.csv`: mode, seed, time, slice, offered_mbps, throughput_mbps, latency_ms, packet_loss_percent, jitter_ms, resource_percent, utilization_percent, qos_satisfied
* `results/static_results.csv`, `results/dynamic_results.csv`: per-run, per-slice totals
* `results/summary_statistics.csv`: mean/std over seeds
* `plots/`: plot1 throughput, plot2 latency (log y), plot3 loss, plot4 resource allocation (all vs time, static and dynamic panels);
  plot5-7 static vs dynamic bars per slice and traffic phase (throughput, latency, loss); plot8/8b QoS satisfaction.
  Empty latency cells (no packet received in a window) are NaN and shown as gaps.

## Reproducibility
`--seed=N` sets `RngSeedManager::SetSeed(N)` (run number 1). UE positions are deterministic. Default: seed 1 (run_all uses 1, 2, 3).
Defaults: 50 s timeline, 2/2/10 UEs, 10 MHz at 3.5 GHz, numerology 1, gNB 30 dBm, 1 ms core delay, capacity 50 Mbps, packet sizes 1400/100/100 B.

## Limitations
* Slice control is a core-ingress policer, not PRB-level scheduling; no radio-level isolation exists between slices.
* Downlink only; single gNB, static UEs, UDP only, one scheduler (TDMA-RR).
* Loss uses source-side offered counters; jitter is a per-packet average of FlowMonitor's jitterSum.
* The API calls were written for NR v2.4 / ns-3.38 and are unverified until `check_api.sh` and the build pass.
