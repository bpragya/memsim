"""Consistency checks over a run set from SCRIPTS/run_bard_all.sh (statistics only).

usage: python3 SCRIPTS/check_bard_runs.py <outroot>

For every <outroot>/<set>/<trace>_warmup.txt it checks that every LLC writeback
reached DRAM (or is still waiting), the MSHR peak is <= 128, every core ran its
25M instructions, every write drain emptied the queue from N-8 down to 8 (so at
least N-16 writes), write BLP <= 32 banks and the mean WR-to-WR gap >= 8 cycles
(tCCD_S_WR). Warmup-only runs keep all their statistics in the warmup files.
"""
import glob, os, re, sys

root = sys.argv[1]
fails = 0
n = 0
for path in sorted(glob.glob(os.path.join(root, '*', '*_warmup.txt'))):
    t = open(path).read()
    cfg = os.path.basename(os.path.dirname(path))
    def vals(k):
        return [float(v) for v in re.findall(r'^\s*' + k + r'\s*:\s*([-\d.]+)\s*$', t, re.M)]
    def one(k):
        return vals(k)[0]
    n += 1
    probs = []
    # every LLC writeback was accepted by DRAM or is still waiting
    if not (one('L3CACHE_WRITEBACKS') == one('MEM_WB_SENT') == one('total_num_write_requests') + one('MEM_WBBUF_LEFT')):
        probs.append('writebacks lost')
    if one('MEM_MSHR_MAX') > 128:
        probs.append('MSHR peak above 128')
    inst = vals(r'CORE_\d\d_INST')
    if min(inst) < 25_000_000:
        probs.append(f'a core ran only {min(inst):.0f} instructions')
    wrq = 48 if cfg == 'alone' else int(cfg[len('baseline_wrq'):])
    for drains, writes, banks, gaps, gtot in zip(vals('write_drains'), vals('drain_writes'), vals('drain_banks_total'),
                                                 vals('drain_wr2wr_gaps'), vals('drain_wr2wr_total')):
        if drains and writes < (wrq - 16) * drains:   # each drain moves the queue from N-8 down to 8
            probs.append(f'drains too short: {writes / drains:.1f} writes/drain < {wrq - 16}')
        if drains and banks / drains > 32:
            probs.append('BLP above 32')
        if gaps and gtot / gaps < 8:
            probs.append('WR->WR below 8 cycles')
    if probs:
        fails += 1
        print(f'{cfg}/{os.path.basename(path)}: ' + '; '.join(sorted(set(probs))))
print(f'{n} runs checked, {fails} with problems -> {"PASS" if fails == 0 else "FAIL"}')
