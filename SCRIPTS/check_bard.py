"""Check memsim's BARD LLC decisions against the paper (Vittal & Qureshi, HPCA 2026).

usage: python3 SCRIPTS/check_bard.py <bardlog> [pbpl 0|1, default 1]

<bardlog> comes from memsim -bardlog: one line per LLC eviction, with the set as
it was before the decision. Independently of memsim's code, this script
  - recomputes each line's DRAM bank from its address (AMD Zen + PBPL, BARD
    Fig. 6; same formula as SCRIPTS/check_mapping/zen_ref.py),
  - replays the BLP-Tracker from all zeros (Sec. IV-A): a bank's bit is set on
    every LLC writeback; once all bits of a sub-channel are 1 they are cleared,
  - redoes every decision: LRU when BARD is off; BARD-E (Sec. IV-B): if the LRU
    line is dirty and its bank's bit is set, evict the first dirty line from
    LRU to MRU whose bank's bit is 0, else the LRU line,
and counts every difference from what memsim logged.
"""
import sys


def bits(a, lo, n):
    return (a >> lo) & ((1 << n) - 1)


def zen_bank(addr, pbpl, banks_per_subch):
    """Flat bank of a byte address: sub-channel * banks_per_subch + bank group * 4 + bank."""
    ch, bg, ba = bits(addr, 6, 1), bits(addr, 8, 3), bits(addr, 11, 2)
    if pbpl:
        bg ^= bits(addr, 20, 3)
        ba ^= bits(addr, 23, 2)
    return ch * banks_per_subch + bg * 4 + ba


path = sys.argv[1]
pbpl = len(sys.argv) < 3 or sys.argv[2] == '1'
names = {0: 'off (LRU)', 1: 'BARD-E', 2: 'BARD-C', 3: 'BARD-H'}

tracker = 0
n = 0
bad = {'bank': 0, 'tracker': 0, 'victim': 0, 'action': 0}
count = {'L': 0, 'E': 0, 'dirty_lru': 0, 'dirty_lru_pending': 0, 'no_candidate': 0, 'writebacks': 0, 'resets': 0}
examples = []

with open(path) as f:
    head = f.readline().split()   # # bard <p> assoc <a> banks <b> banks_per_subch <s>
    hdr = dict(zip(head[1::2], map(int, head[2::2])))
    bard, assoc, per_sub = hdr['bard'], hdr['assoc'], hdr['banks_per_subch']
    for line in f:
        fields = line.split()
        logged_tracker = int(fields[1], 16)
        logged_way, action = int(fields[2]), fields[3]
        ways = []
        for w in fields[4:4 + assoc]:
            la, dirty, lineaddr, bank = map(int, w.split(','))
            ways.append((la, dirty, lineaddr, bank))
        n += 1

        banks = [zen_bank(lineaddr * 64, pbpl, per_sub) for (_, _, lineaddr, _) in ways]
        bad['bank'] += sum(b != w[3] for b, w in zip(banks, ways))
        if logged_tracker != tracker:
            bad['tracker'] += 1
            if len(examples) < 5:
                examples.append(f'event {n}: tracker {logged_tracker:016x}, replay {tracker:016x}')
            tracker = logged_tracker   # resynchronize so one slip is reported once

        def pending(way):
            return (tracker >> banks[way]) & 1

        order = sorted(range(assoc), key=lambda w: (ways[w][0], w))   # LRU first
        lru = order[0]
        victim = lru
        if ways[lru][1]:
            count['dirty_lru'] += 1
            if pending(lru):
                count['dirty_lru_pending'] += 1
        if bard in (1, 3) and ways[lru][1] and pending(lru):
            cand = [w for w in order[1:] if ways[w][1] and not pending(w)]
            if cand:
                victim = cand[0]
            else:
                count['no_candidate'] += 1

        expected_action = 'L' if victim == lru else 'E'
        count[expected_action] += 1
        if logged_way != victim:
            bad['victim'] += 1
            if len(examples) < 5:
                examples.append(f'event {n}: memsim evicted way {logged_way}, expected {victim}')
        if action != expected_action:
            bad['action'] += 1

        if ways[victim][1]:   # the evicted line is written back
            count['writebacks'] += 1
            b = banks[victim]
            mask = ((1 << per_sub) - 1) << (b // per_sub * per_sub)
            tracker |= 1 << b
            if tracker & mask == mask:
                tracker &= ~mask
                count['resets'] += 1

print(f'{path}: {names.get(bard, bard)}, {n} evictions checked')
print(f"  LRU evictions {count['L']}, BARD-E overrides {count['E']}; dirty LRU victims {count['dirty_lru']}, "
      f"of which to a bank with a pending write {count['dirty_lru_pending']} "
      f"(no other dirty line to an idle bank: {count['no_candidate']})")
print(f"  writebacks {count['writebacks']}, BLP-Tracker sub-channel resets {count['resets']}")
print(f"  mismatches: bank {bad['bank']}, tracker {bad['tracker']}, evicted way {bad['victim']}, "
      f"L/E label {bad['action']}")
for e in examples:
    print('  ' + e)
ok = n > 0 and not any(bad.values())
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
