"""Independent reference for AMD Zen + PBPL, written from BARD Fig. 6 (physical bits),
not from the C++ mapper. Compares against map_check output.
usage: zen_ref.py <map_check output> <pbpl 0|1>
"""
import sys
from collections import defaultdict


def bits(a, lo, n):
    return (a >> lo) & ((1 << n) - 1)


def zen(addr, pbpl):
    ch = bits(addr, 6, 1)                       # sub-channel
    col = bits(addr, 7, 1) | (bits(addr, 13, 6) << 1)
    bg = bits(addr, 8, 3)
    ba = bits(addr, 11, 2)
    row = addr >> 19
    if pbpl:                                     # XOR with LLC tag bits 20.. (row bit 1..)
        bg ^= bits(addr, 20, 3)
        ba ^= bits(addr, 23, 2)
    return ch, bg, ba, row, col


path, pbpl = sys.argv[1], sys.argv[2] == '1'
levels = None
n = bad = 0
banks = defaultdict(set)
for line in open(path):
    if line.startswith('# levels:'):
        levels = line.split(':')[1].split()
        continue
    f = list(map(int, line.split()))
    addr, vec = f[0], f[1:]
    got = dict(zip(levels, vec))
    exp = zen(addr, pbpl)
    have = (got['Channel'], got['BankGroup'], got['Bank'], got['Row'], got['Column'])
    n += 1
    if have != exp or got.get('Rank', 0) != 0:
        bad += 1
        if bad <= 5:
            print('MISMATCH', addr, 'ramulator2', have, 'reference', exp)
    banks[got['Channel']].add((got['BankGroup'], got['Bank']))
print(f"pbpl={int(pbpl)}: {n} addresses, {bad} mismatches; banks hit per channel: "
      + ", ".join(f"ch{c}={len(b)}" for c, b in sorted(banks.items())))

# same-LLC-set spread: 16MB 16-way 64B LLC -> set index = bits 6..19
sets = defaultdict(set)
for line in open(path):
    if line.startswith('#'):
        continue
    f = list(map(int, line.split()))
    addr, vec = f[0], f[1:]
    got = dict(zip(levels, vec))
    sets[bits(addr, 6, 14)].add((got['Channel'], got['BankGroup'], got['Bank']))
multi = [s for s in sets.values() if len(s) > 0]
print(f"  same-LLC-set lines: avg distinct banks per set = {sum(len(s) for s in multi)/len(multi):.2f}")
