#!/bin/bash
# Run one trace on memsim + Ramulator2 with BARD's Table II baseline system.
# Run in WSL (after SCRIPTS/build_wsl.sh), from anywhere in the repo:
#
#   bash SCRIPTS/run_bard.sh <trace> <outdir> [extra memsim flags]
#
#   <trace>   name in TRACES/ without .mtf.gz, e.g. lbm_17
#   <outdir>  results go to <outdir>/<trace>.txt, warmup stats to
#             <outdir>/<trace>_warmup.txt
#   extra flags come after the Table II ones, so they override them,
#   e.g. "-warmup 1000000 -inst_limit 2000000" for a quick test.
#
# Environment: CONFIG=<yaml> (default configs/bard_table2_wrq48.yaml, made by
# MEMSIM_CONFIG=table2 python -m ramulator export SCRIPTS/ramulator2_config.py),
# BIN=<memsim binary> (default ~/build/bin/memsim_ram2).
#
# Table II (memsim side): 8 cores in rate mode, 16 MB 16-way LRU LLC,
# 512-entry ROB, 128 LLC MSHRs, 25M warmup + 100M measured instructions.
# The DRAM side (DDR5-4800B x4, 2 sub-channels x 32 banks, Zen + PBPL,
# adaptive open-page, 64-entry read / 48-entry write queue, drain 40 -> 8,
# FR-FCFS) is in the config.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
trace=$1; out=$2; shift 2
CONFIG=${CONFIG:-$REPO/configs/bard_table2_wrq48.yaml}
BIN=${BIN:-$HOME/build/bin/memsim_ram2}
TABLE2="-ratemode 8 -l3sizemb 16 -l3assoc 16 -robsize 512 -llcmshrs 128 -warmup 25000000 -inst_limit 100000000"

mkdir -p "$out"
cd "$REPO"
"$BIN" $TABLE2 -warmupfile "$out/${trace}_warmup.txt" -ramulatorconfig "$CONFIG" "$@" \
  TRACES/$trace.mtf.gz > "$out/$trace.txt"
