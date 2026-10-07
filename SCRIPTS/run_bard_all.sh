#!/bin/bash
# Run the full BARD baseline set with SCRIPTS/run_bard.sh, several runs at a time.
# Run in WSL from anywhere in the repo:
#
#   bash SCRIPTS/run_bard_all.sh <outroot> [extra memsim flags]
#
# Runs, for each workload in $WORKLOADS:
#   <outroot>/baseline_wrq<N>/<trace>.txt   8 cores, Table II, write queue N (each N in $WRQS)
#   <outroot>/alone/<trace>.txt             1 core, Table II, 48-entry write queue
#                                           (single-core IPC for weighted speedup)
# plus the matching <trace>_warmup.txt files. Extra flags go to every run, e.g.
#   "-warmup 25000000 -inst_limit 0"   warmup only (stats in the _warmup files)
#
# Environment: WORKLOADS, WRQS, JOBS (parallel runs, default 8).
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
out=$1; shift
WORKLOADS=${WORKLOADS:-"cam4_17 roms_17 omnetpp_17 bwaves_17 wrf_17 fotonik3d_17 lbm_17"}
WRQS=${WRQS:-"32 48 64 96 128"}
JOBS=${JOBS:-8}
mkdir -p "$out"
extra="$*"

jobs_file="$out/jobs.txt"
: > "$jobs_file"
for n in $WRQS; do
  for t in $WORKLOADS; do
    echo "CONFIG=$REPO/configs/bard_table2_wrq$n.yaml bash $REPO/SCRIPTS/run_bard.sh $t $out/baseline_wrq$n $extra" >> "$jobs_file"
  done
done
for t in $WORKLOADS; do
  echo "CONFIG=$REPO/configs/bard_table2_wrq48.yaml bash $REPO/SCRIPTS/run_bard.sh $t $out/alone $extra -ratemode 1" >> "$jobs_file"
done

echo "$(wc -l < "$jobs_file") runs, $JOBS at a time; started $(date)"
xargs -P "$JOBS" -I{} bash -c "{}" < "$jobs_file" || true
echo "finished $(date)"

# a run is complete if its output ends with the final report or the warmup-only note
bad=0
for f in "$out"/*/*.txt; do
  case "$f" in *_warmup.txt|*/jobs.txt) continue ;; esac
  if ! grep -qE "SYS_CYCLES|Warmup-only run" "$f"; then
    echo "INCOMPLETE: $f"; bad=$((bad + 1))
  fi
done
echo "incomplete runs: $bad"
