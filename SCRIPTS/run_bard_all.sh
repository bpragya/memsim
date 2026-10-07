#!/bin/bash
# Run the BARD study with SCRIPTS/run_bard.sh, several runs at a time.
# Run in WSL from anywhere in the repo:
#
#   bash SCRIPTS/run_bard_all.sh <outroot> [extra memsim flags]
#
# Runs, for each workload in $WORKLOADS:
#   <outroot>/baseline_wrq<N>/<trace>.txt  8 cores, Table II, LRU, write queue N (each N in $WRQS)
#   <outroot>/bard<X>_wrq48/<trace>.txt    same with BARD-X, X = E/C/H (each in $POLICIES: 1 2 3)
#   <outroot>/bardH_wrq<N>/<trace>.txt     BARD-H with write queue N (each N in $BARDH_WRQS)
#   <outroot>/alone/<trace>.txt            1 core, Table II, LRU, 48-entry write queue
#                                          (single-core IPC for weighted speedup)
# plus the matching <trace>_warmup.txt files. Extra flags go to every run, e.g.
#   "-warmup 25000000 -inst_limit 0"   warmup only (stats in the _warmup files)
#
# Environment (defaults = the full study):
#   WORKLOADS  (7 SPEC traces)   WRQS ("32 48 64 96 128")   POLICIES ("1 2 3")
#   BARDH_WRQS ("32 64 96 128")  JOBS (parallel runs, 8)  DRYRUN=1 (only list the runs)
# e.g. WRQS=48 BARDH_WRQS= WORKLOADS="lbm_17 fotonik3d_17" for BARD at 48 entries only.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
out=$1; shift
WORKLOADS=${WORKLOADS:-"cam4_17 roms_17 omnetpp_17 bwaves_17 wrf_17 fotonik3d_17 lbm_17"}
WRQS=${WRQS-"32 48 64 96 128"}
POLICIES=${POLICIES-"1 2 3"}
BARDH_WRQS=${BARDH_WRQS-"32 64 96 128"}
JOBS=${JOBS:-8}
mkdir -p "$out"
extra="$*"
names=(baseline bardE bardC bardH)

# (policy, write-queue size) pairs, without repeats
runs=""
add () { case " $runs " in *" $1 "*) ;; *) runs="$runs $1" ;; esac; }
for n in $WRQS; do add "0:$n"; done
for p in $POLICIES; do add "$p:48"; done
for n in $BARDH_WRQS; do add "3:$n"; done

jobs_file="$out/jobs.txt"
: > "$jobs_file"
for t in $WORKLOADS; do   # single-core runs first: they are short
  echo "CONFIG=$REPO/configs/bard_table2_wrq48.yaml bash $REPO/SCRIPTS/run_bard.sh $t $out/alone $extra -ratemode 1" >> "$jobs_file"
done
for r in $runs; do
  p=${r%:*}; n=${r#*:}
  for t in $WORKLOADS; do
    echo "CONFIG=$REPO/configs/bard_table2_wrq$n.yaml bash $REPO/SCRIPTS/run_bard.sh $t $out/${names[$p]}_wrq$n $extra -bard $p" >> "$jobs_file"
  done
done

if [ -n "$DRYRUN" ]; then cat "$jobs_file"; exit 0; fi
echo "$(wc -l < "$jobs_file") runs, $JOBS at a time; started $(date)"
xargs -P "$JOBS" -I{} bash -c "{}" < "$jobs_file" || true
echo "finished $(date)"

# a run is complete if its output ends with the final report or the warmup-only note
bad=0
for f in "$out"/*/*.txt; do
  case "$f" in *_warmup.txt|*/jobs.txt|*/report.txt) continue ;; esac
  if ! grep -qE "SYS_CYCLES|Warmup-only run" "$f"; then
    echo "INCOMPLETE: $f"; bad=$((bad + 1))
  fi
done
echo "incomplete runs: $bad"
