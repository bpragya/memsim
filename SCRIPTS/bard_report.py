"""Summarize a BARD run set made by SCRIPTS/run_bard_all.sh.

usage: python3 SCRIPTS/bard_report.py <outroot>      (also writes <outroot>/report.txt)

Reads <outroot>/baseline_wrq<N>/<trace>.txt (8 cores) and <outroot>/alone/
<trace>.txt (1 core). For a warmup-only run (-inst_limit 0) the statistics
come from <trace>_warmup.txt instead.

Per workload (48-entry write queue):
  MPKI, WPKI        LLC misses / writebacks per 1000 instructions (all cores);
                    WPKI counts dirty evictions (L3CACHE_WRITEBACKS) only, not
                    BARD-C/H cleanses (L3CACHE_BARD_CLEANSES), which are DRAM writes too
  write BLP         distinct banks written per write drain (max 32)
  write %           cycles in write mode with writes queued, % of all cycles
  WR->WR ns         mean gap between writes inside a drain (DRAM cycles x tCK)
  WQ occ.           mean write-queue occupancy (entries)
  WS                weighted speedup = sum over cores of IPC / single-core IPC
Write-queue sweep: WS(N) / WS(48) - 1 per workload and geometric mean, as in
the paper's Fig. 17. Paper columns are BARD (HPCA 2026) Table IV / Fig. 17.
"""
import glob
import math
import os
import re
import sys

TCK_NS = 0.416  # DDR5-4800

# BARD Table IV (8-core baseline): MPKI, WPKI, write BLP, write %
PAPER_T4 = {
    'cam4_17': (9.2, 4.1, 21.6, 43.9), 'roms_17': (13.2, 2.7, 11.4, 26.3),
    'omnetpp_17': (13.7, 5.5, 17.9, 22.7), 'bwaves_17': (20.8, 6.1, 23.4, 39.3),
    'wrf_17': (25.4, 7.3, 22.7, 33.1), 'fotonik3d_17': (30.6, 9.7, 23.9, 36.9),
    'lbm_17': (48.5, 25.5, 24.6, 51.8),
}
PAPER_FIG17 = {32: -6.2, 48: 0.0, 64: 3.3, 96: 8.1, 128: 10.7}  # baseline speedup %, all workloads


def stats_text(path):
    """The run's statistics, or None if it has not finished."""
    txt = open(path).read()
    if 'Warmup-only run' in txt:
        return open(path[:-4] + '_warmup.txt').read()
    return txt if 'SYS_CYCLES' in txt else None


def values(txt, key):
    return [float(v) for v in re.findall(r'^\s*' + key + r'\s*:\s*([-\d.eE+]+)\s*$', txt, re.M)]


def run_stats(path):
    t = stats_text(path)
    if t is None:
        return None
    inst = values(t, r'CORE_\d\d_INST')
    s = dict(
        ipc=values(t, r'CORE_\d\d_IPC'),
        inst=sum(inst),
        miss=sum(values(t, r'CORE_\d\d_MISS')),
        wb=sum(values(t, 'L3CACHE_WRITEBACKS')),
        drains=sum(values(t, 'write_drains')),
        banks=sum(values(t, 'drain_banks_total')),
        gaps=sum(values(t, 'drain_wr2wr_gaps')),
        gap_total=sum(values(t, 'drain_wr2wr_total')),
        wmode=sum(values(t, 'write_mode_cycles')),
        cycles=sum(values(t, 'cycles')),
        wq=values(t, 'write_queue_len_avg'),
    )
    k = 1000.0 / s['inst'] if s['inst'] else 0
    s.update(
        mpki=s['miss'] * k, wpki=s['wb'] * k,
        blp=s['banks'] / s['drains'] if s['drains'] else None,    # undefined without drains
        wpct=100.0 * s['wmode'] / s['cycles'] if s['cycles'] else 0,
        w2w=TCK_NS * s['gap_total'] / s['gaps'] if s['gaps'] else None,
        wqocc=sum(s['wq']) / len(s['wq']) if s['wq'] else 0,
    )
    return s


def gmean(xs):
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else None


def fmt(v, width, prec, unit=''):
    """A value, or n/a when it is undefined (None)."""
    return f'{v:{width - len(unit)}.{prec}f}{unit}' if v is not None else f"{'n/a':>{width}}"


def defined(xs):
    return [x for x in xs if x is not None]


root = sys.argv[1]
runs = {}  # config -> trace -> stats
for path in sorted(glob.glob(os.path.join(root, '*', '*.txt'))):
    if path.endswith('_warmup.txt') or os.path.basename(path) in ('jobs.txt', 'report.txt'):
        continue
    cfg, trace = os.path.basename(os.path.dirname(path)), os.path.basename(path)[:-4]
    try:
        st = run_stats(path)
        if st is None:
            print(f'not finished yet: {path}', file=sys.stderr)
            continue
        runs.setdefault(cfg, {})[trace] = st
    except (OSError, ZeroDivisionError) as e:
        print(f'skipping {path}: {e}', file=sys.stderr)

alone = runs.get('alone', {})


def ws(s, trace):
    """Weighted speedup; None without the single-core run."""
    a = alone.get(trace)
    return sum(i / a['ipc'][0] for i in s['ipc']) if s and a and a['ipc'] and a['ipc'][0] else None


out = []
base = runs.get('baseline_wrq48', {})
traces = [t for t in PAPER_T4 if t in base] + sorted(t for t in base if t not in PAPER_T4)
out.append(f'Baseline, 48-entry write queue ({root})')
out.append(f"{'workload':13} {'MPKI':>6} {'WPKI':>6} {'BLP':>6} {'write%':>7} {'WR->WR':>7} {'WQ occ':>7} {'WS':>6}"
           f"   | paper: {'MPKI':>5} {'WPKI':>5} {'BLP':>5} {'write%':>6}")
for t in traces:
    s = base[t]
    p = PAPER_T4.get(t)
    ptxt = f"{p[0]:5.1f} {p[1]:5.1f} {p[2]:5.1f} {p[3]:6.1f}" if p else ''
    out.append(f"{t:13} {s['mpki']:6.1f} {s['wpki']:6.1f} {fmt(s['blp'], 6, 1)} {s['wpct']:7.1f} "
               f"{fmt(s['w2w'], 7, 2, 'n')} {s['wqocc']:7.1f} {fmt(ws(s, t), 6, 2)}   |        {ptxt}")
if traces:
    def mean(k):
        """Mean over the workloads where the value is defined, and how many."""
        vs = defined(base[t][k] for t in traces)
        return (sum(vs) / len(vs) if vs else None), len(vs)
    cols = {k: mean(k) for k in ('mpki', 'wpki', 'blp', 'wpct', 'w2w', 'wqocc')}
    out.append(f"{'mean':13} {fmt(cols['mpki'][0], 6, 1)} {fmt(cols['wpki'][0], 6, 1)} {fmt(cols['blp'][0], 6, 1)} "
               f"{fmt(cols['wpct'][0], 7, 1)} {fmt(cols['w2w'][0], 7, 2, 'n')} {fmt(cols['wqocc'][0], 7, 1)}"
               f"          |  paper avg (all 31 workloads): BLP 22.1, write% 33.0, WR->WR 5.0 ns")
    out.append(f"n/a = undefined (no write drains, or no single-core run). Means are over workloads with a value: "
               f"BLP {cols['blp'][1]} of {len(traces)}, WR->WR {cols['w2w'][1]} of {len(traces)}, others {len(traces)}.")
    out.append("Note: WPKI counts LLC dirty evictions (L3CACHE_WRITEBACKS) only. BARD-C/H cleanses "
               "(L3CACHE_BARD_CLEANSES) are also DRAM writes but are not included in WPKI.")

sizes = sorted(int(c[len('baseline_wrq'):]) for c in runs if c.startswith('baseline_wrq'))
if 48 in sizes and alone:
    out.append('')
    out.append('Write-queue sweep: weighted speedup vs 48 entries (%)')
    out.append(f"{'workload':13} " + ' '.join(f'{n:>7}' for n in sizes))
    ratios = {n: [] for n in sizes}
    for t in traces:
        w48 = ws(base[t], t)
        row = []
        for n in sizes:
            s = runs[f'baseline_wrq{n}'].get(t)
            w = ws(s, t)
            r = w / w48 if w is not None and w48 else None
            ratios[n].append(r)
            row.append(fmt(100 * (r - 1) if r is not None else None, 7, 2))
        out.append(f'{t:13} ' + ' '.join(row))
    gm = {n: gmean(defined(ratios[n])) for n in sizes}
    out.append(f"{'gmean':13} " + ' '.join(fmt(100 * (gm[n] - 1) if gm[n] else None, 7, 2) for n in sizes))
    out.append(f"{'(workloads)':13} " + ' '.join(f'{len(defined(ratios[n])):>7}' for n in sizes))
    out.append(f"{'paper Fig.17':13} " + ' '.join(f"{PAPER_FIG17.get(n, float('nan')):7.1f}" for n in sizes))

text = '\n'.join(out)
print(text)
open(os.path.join(root, 'report.txt'), 'w').write(text + '\n')
