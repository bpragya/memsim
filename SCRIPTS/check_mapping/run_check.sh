#!/bin/bash
# Check Ramulator2's ZenPBPL address mapper against an independent reference
# written from BARD Fig. 6. Run in WSL from anywhere in the repo:
#   bash SCRIPTS/check_mapping/run_check.sh [num_addresses]
#
# map_check.cpp runs addresses through Ramulator2's own channel mapper +
# controller address mapper; zen_ref.py recomputes each one from the paper's
# bit positions and counts mismatches. Expect 0 mismatches, 32 banks per
# channel, and same-LLC-set lines spread over many banks only with PBPL on.
#
# zen_pbpl_x4.yaml / zen_x4.yaml are exported from SCRIPTS/ramulator2_config.py
# (MEMSIM_ADDR_MAP=zen_pbpl / zen) with DDR5_4800B timing and DDR5_16Gb_x4
# chips, the geometry Fig. 6 assumes (8KB rows). Needs Ramulator2 built by
# SCRIPTS/build_wsl.sh.
set -e
DIR=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$DIR/../.." && pwd)
RAM2=$REPO/third_party/ramulator2
N=${1:-200000}
OUT=$HOME/build/bin
mkdir -p $OUT

g++ -O2 -std=c++20 -I$RAM2/src $DIR/map_check.cpp -o $OUT/map_check -L$RAM2 -lramulator -Wl,-rpath,$RAM2

for cfg in zen_pbpl:1 zen:0; do
  name=${cfg%%:*}; pbpl=${cfg##*:}
  $OUT/map_check $DIR/${name}_x4.yaml $N > /tmp/map_${name}_x4.txt
  python3 $DIR/zen_ref.py /tmp/map_${name}_x4.txt $pbpl
done
