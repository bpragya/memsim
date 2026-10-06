"""Check warmup bookkeeping: warmup file + measured output = one plain run.

usage: python3 SCRIPTS/check_warmup.py <plain output> <warmup file> <measured output>

Warmup only changes which counters are reported, not the simulation, so
for a single core, `-warmup W -inst_limit M` must add up to a plain run with
`-inst_limit W+M`: every counter in the warmup file plus the same counter in
the main output equals the plain run's counter. (Exact up to the last few
instructions: a core notices its limit only at the next trace record.)
"""
import re
import sys

plain, warm, meas = (open(p).read() for p in sys.argv[1:4])

KEYS = ['CORE_00_INST', 'CORE_00_CYCLES', 'CORE_00_ACCESS', 'CORE_00_MISS', 'L3CACHE_ACCESS', 'L3CACHE_MISS',
        'L3CACHE_WRITEBACKS', 'MEM_WB_SENT', 'total_num_read_requests', 'total_num_write_requests',
        'row_hits', 'row_misses', 'row_conflicts', 'write_drains', 'drain_writes', 'SYS_CYCLES']


def total(txt, key):
    """Sum of every `key : value` line (Ramulator2 prints one per channel)."""
    vals = re.findall(r'^\s*' + key + r'\s*:\s*(\d+)\s*$', txt, re.M)
    return sum(int(v) for v in vals) if vals else None


worst = 0.0
print(f"{'stat':26} {'warmup':>11} {'+ measured':>11} {'= sum':>11} {'plain run':>11} {'diff':>8}")
for k in KEYS:
    p, w, m = total(plain, k), total(warm, k), total(meas, k)
    if None in (p, w, m):
        print(f'{k:26} (missing)')
        continue
    s = w + m
    rel = abs(s - p) / p if p else 0.0
    worst = max(worst, rel)
    print(f'{k:26} {w:11} {m:11} {s:11} {p:11} {100 * rel:7.4f}%')
print(f'largest difference: {100 * worst:.4f}%  ->  {"PASS" if worst < 0.001 else "FAIL"} (limit 0.1%)')
