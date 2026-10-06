"""Check that no LLC writeback is lost on the way to DRAM.

usage: python3 SCRIPTS/check_writebacks.py <memsim output> [llc_mshrs]

Every dirty LLC eviction (L3CACHE_WRITEBACKS) must be handed to memsys
(MEM_WB_SENT), and every one of those must be accepted by Ramulator2
(total_num_write_requests, which counts coalesced writes too) unless it is
still waiting in memsim's writeback buffer when the run ends
(MEM_WBBUF_LEFT). With llc_mshrs given, also checks that no more LLC misses
were ever in flight than that.
"""
import re
import sys

txt = open(sys.argv[1]).read()


def stat(name):
    m = re.search(r'^\s*' + name + r'\s*:\s*(\d+)', txt, re.M)
    return int(m.group(1))


llc_wb = stat('L3CACHE_WRITEBACKS')
sent, delayed, stalls = stat('MEM_WB_SENT'), stat('MEM_WB_DELAYED'), stat('MEM_WB_STALLS')
left, peak = stat('MEM_WBBUF_LEFT'), stat('MEM_WBBUF_MAX')
dram = stat('total_num_write_requests')
mshr_max, mshr_full = stat('MEM_MSHR_MAX'), stat('MEM_MSHR_FULL')

ok = llc_wb == sent and dram + left == sent
print(f'LLC dirty evictions {llc_wb}, handed to memsys {sent}, accepted by DRAM {dram}, still waiting {left}')
print(f'  writebacks that had to wait for a write-queue slot: {delayed} (peak {peak} waiting); '
      f'reads stalled by a full writeback buffer: {stalls}')
print(f'  LLC misses in flight: peak {mshr_max}; reads refused for lack of an MSHR: {mshr_full}')
if len(sys.argv) > 2:
    cap = int(sys.argv[2])
    ok &= mshr_max <= cap
    print(f'  peak <= MSHR limit {cap}: {mshr_max <= cap}')
print('PASS' if ok else 'FAIL')
