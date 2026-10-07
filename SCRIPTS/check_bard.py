"""Check memsim's BARD LLC decisions against the paper (Vittal & Qureshi, HPCA 2026).

usage: python3 SCRIPTS/check_bard.py <bardlog> [pbpl 0|1, default 1]

<bardlog> comes from memsim -bardlog: one line per LLC eviction, with the set as
it was before the decision. Independently of memsim's code, this script
  - recomputes each line's DRAM bank from its address (AMD Zen + PBPL, BARD
    Fig. 6; same formula as SCRIPTS/check_mapping/zen_ref.py),
  - replays the BLP-Tracker from all zeros (Sec. IV-A): a bank's bit is set on
    every LLC writeback; once all bits of a sub-channel are 1 they are cleared,
  - redoes every decision (pending = the bank's tracker bit is 1):
      off    : evict the LRU line
      BARD-E : (Sec. IV-B) if the LRU line is dirty and pending, evict the first
               dirty line from LRU to MRU that is not pending, else the LRU line
      BARD-C : (Sec. IV-C) if the LRU line is clean, evict it and also write back
               the first dirty line from LRU to MRU that is not pending; that line
               stays in the cache, clean
      BARD-H : (Sec. V-A) BARD-E if the LRU line is dirty, BARD-C if it is clean
  - checks that every cleansed line is still in its set the next time that set
    is logged (it may be dirty again if the program wrote it meanwhile),
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
bad = {'set': 0, 'bank': 0, 'tracker': 0, 'victim': 0, 'cleanse': 0, 'label': 0, 'cleansed_gone': 0}
count = {'L': 0, 'E': 0, 'C': 0, 'dirty_lru': 0, 'dirty_lru_pending': 0, 'no_override': 0,
         'clean_lru': 0, 'no_cleanse': 0, 'writebacks': 0, 'resets': 0,
         'cleansed_seen': 0, 'cleansed_dirty_again': 0}
examples = []
cleansed = {}   # set -> line address cleansed at that set's last logged eviction


def example(msg):
    if len(examples) < 5:
        examples.append(msg)


with open(path) as f:
    head = f.readline().split()   # # bard <p> assoc <a> banks <b> banks_per_subch <s> sets <n>
    hdr = dict(zip(head[1::2], map(int, head[2::2])))
    bard, assoc, per_sub, nsets = hdr['bard'], hdr['assoc'], hdr['banks_per_subch'], hdr['sets']
    for line in f:
        fields = line.split()
        s = int(fields[0])
        logged_tracker = int(fields[1], 16)
        logged_way, action, logged_cleanse = int(fields[2]), fields[3], int(fields[4])
        ways = []
        for w in fields[5:5 + assoc]:
            la, dirty, lineaddr, bank = map(int, w.split(','))
            ways.append((la, dirty, lineaddr, bank))
        n += 1

        bad['set'] += sum(lineaddr % nsets != s for (_, _, lineaddr, _) in ways)
        banks = [zen_bank(lineaddr * 64, pbpl, per_sub) for (_, _, lineaddr, _) in ways]
        bad['bank'] += sum(b != w[3] for b, w in zip(banks, ways))
        if logged_tracker != tracker:
            bad['tracker'] += 1
            example(f'event {n}: tracker {logged_tracker:016x}, replay {tracker:016x}')
            tracker = logged_tracker   # resynchronize so one slip is reported once

        if s in cleansed:   # the line cleansed at this set's previous eviction
            present = [w for w in ways if w[2] == cleansed[s]]
            if present:
                count['cleansed_seen'] += 1
                count['cleansed_dirty_again'] += present[0][1]
            else:
                bad['cleansed_gone'] += 1
                example(f'event {n}: cleansed line {cleansed[s]} no longer in set {s}')
            del cleansed[s]

        def pending(way):
            return (tracker >> banks[way]) & 1

        order = sorted(range(assoc), key=lambda w: (ways[w][0], w))   # LRU first
        lru = order[0]
        victim, cleanse = lru, -1
        if ways[lru][1]:
            count['dirty_lru'] += 1
            if pending(lru):
                count['dirty_lru_pending'] += 1
                if bard in (1, 3):
                    cand = [w for w in order[1:] if ways[w][1] and not pending(w)]
                    if cand:
                        victim = cand[0]
                    else:
                        count['no_override'] += 1
        else:
            count['clean_lru'] += 1
            if bard in (2, 3):
                cand = [w for w in order if ways[w][1] and not pending(w)]
                if cand:
                    cleanse = cand[0]
                else:
                    count['no_cleanse'] += 1

        label = 'E' if victim != lru else ('C' if cleanse >= 0 else 'L')
        count[label] += 1
        if logged_way != victim:
            bad['victim'] += 1
            example(f'event {n}: memsim evicted way {logged_way}, expected {victim}')
        if logged_cleanse != cleanse:
            bad['cleanse'] += 1
            example(f'event {n}: memsim cleansed way {logged_cleanse}, expected {cleanse}')
        if action != label:
            bad['label'] += 1

        for w in ([victim] if ways[victim][1] else []) + ([cleanse] if cleanse >= 0 else []):
            count['writebacks'] += 1   # evicted dirty line, then the cleansed line
            b = banks[w]
            mask = ((1 << per_sub) - 1) << (b // per_sub * per_sub)
            tracker |= 1 << b
            if tracker & mask == mask:
                tracker &= ~mask
                count['resets'] += 1
        if cleanse >= 0:
            cleansed[s] = ways[cleanse][2]

c = count
print(f'{path}: {names.get(bard, bard)}, {n} evictions checked')
print(f"  evictions: LRU {c['L']}, BARD-E overrides {c['E']}, BARD-C cleanses {c['C']}")
print(f"  dirty LRU victims {c['dirty_lru']} (bank pending {c['dirty_lru_pending']}, no override possible "
      f"{c['no_override']}); clean LRU victims {c['clean_lru']} (nothing to cleanse {c['no_cleanse']})")
print(f"  writebacks {c['writebacks']}, BLP-Tracker sub-channel resets {c['resets']}")
if c['C']:
    print(f"  cleansed lines seen again in their set: {c['cleansed_seen']}, "
          f"of which written again (dirty) {c['cleansed_dirty_again']}")
print('  mismatches: ' + ', '.join(f'{k} {v}' for k, v in bad.items()))
for e in examples:
    print('  ' + e)
ok = n > 0 and not any(bad.values())
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
