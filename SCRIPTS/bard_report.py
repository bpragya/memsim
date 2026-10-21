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
Write-queue sweep (when baseline_wrq<N> sets for other N exist): WS(N) / WS(48)
- 1 per workload and geometric mean, as in the paper's Fig. 17.
BARD vs baseline (when bardE/bardC/bardH_wrq48 sets exist): the same statistics
per policy, speedup = WS / baseline WS - 1 (geometric mean over workloads, as
Fig. 10), and the eviction breakdown (LRU / BARD-E override / BARD-C cleanse).
Paper numbers are from BARD (HPCA 2026) Table IV, Table V, Fig. 10, 14 and 17.
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
# run-set prefixes and labels; ...rr = same LLC policy with the BankRR memory controller
POLICIES = [('baseline', 'baseline'), ('bardE', 'BARD-E'), ('bardC', 'BARD-C'), ('bardH', 'BARD-H'),
            ('rr', 'BankRR'), ('bardErr', 'RR+BARD-E'), ('bardCrr', 'RR+BARD-C'), ('bardHrr', 'RR+BARD-H')]


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
        evictions=sum(values(t, 'L3CACHE_EVICTIONS')),
        lru_ev=sum(values(t, 'L3CACHE_LRU_EVICTS')),
        overrides=sum(values(t, 'L3CACHE_BARD_OVERRIDES')),
        cleanses=sum(values(t, 'L3CACHE_BARD_CLEANSES')),
        wlat_total=values(t, 'write_latency'),   # empty in runs made before these stats existed
        wserved=sum(values(t, 'num_write_reqs_served')),
        same_bg=values(t, 'drain_wr_same_bg'),
    )
    k = 1000.0 / s['inst'] if s['inst'] else 0
    s.update(
        mpki=s['miss'] * k, wpki=s['wb'] * k,
        blp=s['banks'] / s['drains'] if s['drains'] else None,    # undefined without drains
        wpct=100.0 * s['wmode'] / s['cycles'] if s['cycles'] else 0,
        w2w=TCK_NS * s['gap_total'] / s['gaps'] if s['gaps'] else None,
        wqocc=sum(s['wq']) / len(s['wq']) if s['wq'] else 0,
        # write latency: write-queue arrival to WR issue, mean over all writes (ns)
        wlat=TCK_NS * sum(s['wlat_total']) / s['wserved'] if s['wlat_total'] and s['wserved'] else None,
        # back-to-back WRs in a drain that share a bank group, % of WR->WR gaps
        samebg=100.0 * sum(s['same_bg']) / s['gaps'] if s['same_bg'] and s['gaps'] else None,
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
if 48 in sizes and len(sizes) > 1 and alone:
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

pols = [(k, name) for k, name in POLICIES if f'{k}_wrq48' in runs]
if base and len(pols) > 1:
    out.append('')
    out.append('Policies vs baseline, 48-entry write queue (BankRR = round-robin memory controller; '
               'speedup = WS / baseline WS - 1; evictions = LRU / BARD-E override / BARD-C cleanse, % of all evictions)')
    out.append(f"{'workload':13} {'policy':9} {'MPKI':>6} {'WPKI':>6} {'BLP':>6} {'write%':>7} {'WR->WR':>7} "
               f"{'WQ occ':>7} {'WR lat':>8} {'sameBG%':>7} {'WS':>6} {'speedup':>8}   {'LRU%':>6} {'E%':>6} {'C%':>6}")
    speed = {k: [] for k, _ in pols}
    for t in traces:
        base_ws = ws(base[t], t)
        for i, (k, name) in enumerate(pols):
            label = t if i == 0 else ''
            s = runs[f'{k}_wrq48'].get(t)
            if s is None:
                out.append(f'{label:13} {name:9} not run')
                continue
            w = ws(s, t)
            r = w / base_ws if w is not None and base_ws else None
            if k != 'baseline':
                speed[k].append(r)
            ev = s['evictions']
            br = [100.0 * s[x] / ev if ev else None for x in ('lru_ev', 'overrides', 'cleanses')]
            out.append(f"{label:13} {name:9} {s['mpki']:6.1f} {s['wpki']:6.1f} {fmt(s['blp'], 6, 1)} {s['wpct']:7.1f} "
                       f"{fmt(s['w2w'], 7, 2, 'n')} {s['wqocc']:7.1f} {fmt(s['wlat'], 8, 1, 'n')} {fmt(s['samebg'], 7, 2)} "
                       f"{fmt(w, 6, 2)} {fmt(100 * (r - 1) if r is not None else None, 7, 2, '%')}"
                       f"   {fmt(br[0], 6, 1)} {fmt(br[1], 6, 1)} {fmt(br[2], 6, 1)}")
    out.append("Note: WPKI counts dirty evictions only; BARD-C/H cleanses (C%) are DRAM writes too and are not in WPKI.")
    out.append("WR lat = mean time from a write entering the controller's write queue to its WR command (ns); "
               "sameBG% = back-to-back WRs in a drain to the same bank group, % of WR->WR gaps (n/a: older runs).")
    out.append('')
    out.append(f"{'summary':13} {'policy':9} {'gmean speedup':>14} {'mean BLP':>9} {'mean write%':>12} "
               f"{'mean WR->WR':>12} {'max WR->WR':>11} {'mean WR lat':>12}   (over {len(traces)} workloads)")
    def avg(xs):
        """Mean over the defined values (None if there are none)."""
        xs = defined(xs)
        return sum(xs) / len(xs) if xs else None
    for k, name in pols:
        ss = [runs[f'{k}_wrq48'][t] for t in traces if t in runs[f'{k}_wrq48']]
        g = gmean(defined(speed[k])) if k != 'baseline' else 1.0
        w2ws = defined(s['w2w'] for s in ss)
        out.append(f"{'':13} {name:9} {fmt(100 * (g - 1) if g else None, 13, 2, '%')} "
                   f"{fmt(avg(s['blp'] for s in ss), 9, 1)} {fmt(avg(s['wpct'] for s in ss), 12, 1)} "
                   f"{fmt(avg(w2ws), 12, 2, 'n')} {fmt(max(w2ws) if w2ws else None, 11, 2, 'n')} "
                   f"{fmt(avg(s['wlat'] for s in ss), 12, 1, 'n')}")
    out.append("paper (31 workloads; 'BARD' = BARD-H): gmean speedup BARD-E 4.1%, BARD-C 3.3%, BARD-H 4.3% (Fig. 10); "
               "BLP 22.1 -> 28.8, write% 33.0 -> 29.3 (Fig. 14);")
    out.append("  WR->WR average 5.0 -> 4.2 ns, Table V 'max' 5.7 -> 5.0 ns; BARD-H evictions "
               "64.7% LRU / 4.8% E / 30.5% C (Fig. 10).")

text = '\n'.join(out)
print(text)
open(os.path.join(root, 'report.txt'), 'w').write(text + '\n')
