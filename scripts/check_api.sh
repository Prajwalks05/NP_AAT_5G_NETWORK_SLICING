#!/usr/bin/env bash
# Phase 0: inspect installed ns-3 / 5G-LENA and check that every API used by
# scratch/dynamic-slicing.cc exists. Usage: scripts/check_api.sh /path/to/ns-3-dev
set -u
NS3=${1:-$HOME/ns-3-dev}
NR="$NS3/contrib/nr"
echo "== ns-3 version ==";   grep -m1 -E "version|VERSION" "$NS3/VERSION" 2>/dev/null || cat "$NS3/VERSION"
echo "== 5G-LENA version =="; (cd "$NR" && git describe --tags --always 2>/dev/null; git branch --show-current 2>/dev/null)
grep -m3 -iE "^## Release|ns-3\.[0-9]+" "$NR/CHANGES.md" 2>/dev/null | head -3
echo "== NR examples ==";    ls "$NR/examples" | head -30
fail=0
chk() { # file-glob  pattern  description
  if grep -rqE "$2" $1 2>/dev/null; then echo "OK    $3"; else echo "MISSING $3   (pattern: $2)"; fail=1; fi
}
H="$NR/helper"; M="$NR/model"
chk "$H/nr-helper.h"                    "SetEpcHelper"                         "NrHelper::SetEpcHelper"
chk "$H/nr-helper.h"                    "SetBeamformingHelper"                 "NrHelper::SetBeamformingHelper"
chk "$H/nr-helper.h"                    "InitializeOperationBand"              "NrHelper::InitializeOperationBand"
chk "$H/nr-helper.h"                    "InstallGnbDevice"                     "NrHelper::InstallGnbDevice"
chk "$H/nr-helper.h"                    "InstallUeDevice"                      "NrHelper::InstallUeDevice"
chk "$H/nr-helper.h"                    "AttachToClosestEnb"                   "NrHelper::AttachToClosestEnb"
chk "$H/nr-helper.h"                    "ActivateDedicatedEpsBearer"           "NrHelper::ActivateDedicatedEpsBearer"
chk "$H/nr-helper.h"                    "SetSchedulerTypeId"                   "NrHelper::SetSchedulerTypeId"
chk "$H/nr-helper.h"                    "SetPathlossAttribute"                 "NrHelper::SetPathlossAttribute"
chk "$H/nr-helper.h"                    "SetChannelConditionModelAttribute"    "NrHelper::SetChannelConditionModelAttribute"
chk "$H/nr-helper.h"                    "GetGnbPhy"                            "NrHelper::GetGnbPhy"
chk "$H/nr-helper.h"                    "SetGnbPhyAttribute|GetGnbPhy"         "gNB PHY attribute access"
chk "$H/cc-bwp-helper.h"                "SimpleOperationBandConf"              "CcBwpCreator::SimpleOperationBandConf"
chk "$H/cc-bwp-helper.h"                "BandwidthPartInfo::Scenario|UMi_StreetCanyon" "scenario arg for band conf"
chk "$H/nr-point-to-point-epc-helper.h" "AssignUeIpv4Address"                  "AssignUeIpv4Address"
chk "$H/nr-point-to-point-epc-helper.h" "GetUeDefaultGatewayAddress|class NrPointToPointEpcHelper" "UE default gateway"
chk "$H/ideal-beamforming-helper.h"     "class IdealBeamformingHelper"         "IdealBeamformingHelper"
chk "$M/nr-mac-scheduler-tdma-rr.h"     "class NrMacSchedulerTdmaRR"           "NrMacSchedulerTdmaRR"
chk "$M/nr-gnb-net-device.h"            "UpdateConfig"                         "NrGnbNetDevice::UpdateConfig"
chk "$M/nr-ue-net-device.h"             "UpdateConfig"                         "NrUeNetDevice::UpdateConfig"
chk "$NS3/src/lte/model/epc-tft.h"      "localPortStart"                       "EpcTft::PacketFilter::localPortStart"
chk "$NS3/src/lte/model/eps-bearer.h"   "NGBR_LOW_LAT_EMBB"                    "EpsBearer::NGBR_LOW_LAT_EMBB"
chk "$NS3/src/lte/model/eps-bearer.h"   "NGBR_VIDEO_TCP_PREMIUM"               "EpsBearer::NGBR_VIDEO_TCP_PREMIUM"
chk "$NS3/src/lte/model/eps-bearer.h"   "NGBR_VIDEO_TCP_DEFAULT"               "EpsBearer::NGBR_VIDEO_TCP_DEFAULT"
echo
echo "Reference examples to compare against: $NR/examples/cttc-nr-demo.cc  cttc-3gpp-channel-simple-ran.cc"
[ $fail -eq 0 ] && echo "All checked APIs exist." || echo "Some APIs are missing: adapt scratch/dynamic-slicing.cc to the installed version (compare with cttc-nr-demo.cc)."
exit $fail
