#!/usr/bin/env bash
# Run static + dynamic experiments for several seeds, then the analysis.
# Usage: scripts/run_all.sh /path/to/ns-3-dev [seeds...]     (default seeds: 1 2 3)
set -euo pipefail
NS3=${1:?usage: run_all.sh /path/to/ns-3-dev [seeds]}; shift || true
SEEDS=${*:-1 2 3}
PROJ="$(cd "$(dirname "$0")/.." && pwd)"
cp "$PROJ/scratch/dynamic-slicing.cc" "$NS3/scratch/dynamic-slicing.cc"
mkdir -p "$PROJ/results" "$PROJ/plots"
rm -f "$PROJ"/results/*.csv "$PROJ"/results/*.log
cd "$NS3"
./ns3 build
for seed in $SEEDS; do
  for dyn in 0 1; do
    echo "=== seed=$seed dynamic=$dyn ==="
    ./ns3 run "dynamic-slicing --enableDynamicSlicing=$dyn --seed=$seed --outDir=$PROJ/results --configFile=$PROJ/config/simulation-config.txt" \
      2>&1 | tee "$PROJ/results/run_dyn${dyn}_seed${seed}.log"
  done
done
python3 "$PROJ/analysis/analyze_results.py" --results "$PROJ/results" --plots "$PROJ/plots"
