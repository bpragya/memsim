"""Consistency checks over a run set from SCRIPTS/run_bard_all.sh (statistics only).

usage: python3 SCRIPTS/check_bard_runs.py <outroot> [warmup_inst measured_inst]
       (defaults 25000000 100000000)

Checks every <outroot>/<set>/<trace>_warmup.txt and every finished full-run
<outroot>/<set>/<trace>.txt (sets: baseline_wrqN, bardE_wrqN, bardC_wrqN,
bardH_wrqN, alone = baseline single core, 48 entries):
  - every LLC writeback (dirty evictions + BARD-C cleanses) reached DRAM or is
    still waiting
  - the MSHR peak is <= 128
  - every core ran its instructions (warmup_inst in the warmup file,
    measured_inst in the full-run file)
  - every write drain emptied the queue from N-8 down to 8 (>= N-16 writes)
  - write BLP <= 32 banks, mean WR-to-WR gap >= 8 cycles (tCCD_S_WR)
  - the run used the BARD policy its set name says, and LRU evictions +
    BARD-E overrides + BARD-C cleanses = all evictions
Warmup-only runs keep all their statistics in the warmup files.
"""
import glob, os, re, sys

root = sys.argv[1]
warm_inst = int(sys.argv[2]) if len(sys.argv) > 2 else 25_000_000
meas_inst = int(sys.argv[3]) if len(sys.argv) > 3 else 100_000_000
POLICY = {'baseline': 0, 'alone': 0, 'bardE': 1, 'bardC': 2, 'bardH': 3}

fails = 0
n = 0
for path in sorted(glob.glob(os.path.join(root, '*', '*.txt'))):
    t = open(path).read()
    cfg = os.path.basename(os.path.dirname(path))
    warmup = path.endswith('_warmup.txt')
    if not warmup and 'SYS_CYCLES' not in t:
        continue   # warmup-only run's main file, unfinished run, jobs.txt, report.txt
    def vals(k):
        return [float(v) for v in re.findall(r'^\s*' + k + r'\s*:\s*([-\d.]+)\s*$', t, re.M)]
    def one(k):
        return vals(k)[0]
    n += 1
    probs = []
    llc_wb = one('L3CACHE_WRITEBACKS') + sum(vals('L3CACHE_BARD_CLEANSES'))
    if not (llc_wb == one('MEM_WB_SENT') == one('total_num_write_requests') + one('MEM_WBBUF_LEFT')):
        probs.append('writebacks lost')
    if one('MEM_MSHR_MAX') > 128:
        probs.append('MSHR peak above 128')
    inst = vals(r'CORE_\d\d_INST')
    need = warm_inst if warmup else meas_inst
    if min(inst) < need:
        probs.append(f'a core ran only {min(inst):.0f} instructions (< {need})')
    m = re.search(r'wrq(\d+)$', cfg)
    wrq = int(m.group(1)) if m else 48
    for drains, writes, banks, gaps, gtot in zip(vals('write_drains'), vals('drain_writes'), vals('drain_banks_total'),
                                                 vals('drain_wr2wr_gaps'), vals('drain_wr2wr_total')):
        if drains and writes < (wrq - 16) * drains:   # each drain moves the queue from N-8 down to 8
            probs.append(f'drains too short: {writes / drains:.1f} writes/drain < {wrq - 16}')
        if drains and banks / drains > 32:
            probs.append('BLP above 32')
        if gaps and gtot / gaps < 8:
            probs.append('WR->WR below 8 cycles')
    policy = POLICY.get(cfg.split('_')[0])
    if vals('L3CACHE_BARD'):
        if policy is not None and one('L3CACHE_BARD') != policy:
            probs.append(f'ran BARD policy {one("L3CACHE_BARD"):.0f}, set name says {policy}')
        parts = one('L3CACHE_LRU_EVICTS') + one('L3CACHE_BARD_OVERRIDES') + sum(vals('L3CACHE_BARD_CLEANSES'))
        if parts != one('L3CACHE_EVICTIONS'):
            probs.append('eviction breakdown does not sum to evictions')
    elif policy:
        probs.append('no BARD stats in a BARD run')
    if probs:
        fails += 1
        print(f'{cfg}/{os.path.basename(path)}: ' + '; '.join(sorted(set(probs))))
print(f'{n} files checked, {fails} with problems -> {"PASS" if fails == 0 else "FAIL"}')
