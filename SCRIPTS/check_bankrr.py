"""Check the BankRR controller's write-mode decisions, independently of its code.

usage: python3 SCRIPTS/check_bankrr.py <rr_log>.ch0 [<rr_log>.ch1 ...]

Each log line (Ramulator2 controller param rr_log, one file per sub-channel) is one
write-mode cycle that walked the rotation:
    <clk> <pointer> <chosen position or -1> {pos,flat_bank,head_src,command,ready,opened,row_hit}...
with one entry per bank that has a write. The rules being checked:
  1. order:  position p visits bank (p mod G) * B + p div G, with B banks per bank group and
             G groups (bank 0 of every group, then bank 1, ...): back-to-back positions are in
             different bank groups.
  2. head:   a bank offers its opened write (row opened for it) if it has one, else a row-hit
             write if one is queued, else its oldest write.
  3. choice: walking from the pointer, the first bank whose head is ready is chosen (strict
             rotation: no other priority); none ready -> nothing (-1).
  4. pointer: after a choice the next walk starts at the bank after it; else it stays.
"""
import sys
from collections import Counter

ok = True
for path in sys.argv[1:]:
    with open(path) as f:
        head = f.readline().split()          # # bankrr banks <n> banks_per_group <b>
        hdr = dict(zip(head[2::2], head[3::2]))
        n, bpg = int(hdr['banks']), int(hdr['banks_per_group'])
        groups = n // bpg
        bad = Counter()
        count = Counter()
        prev_ptr_next = None
        examples = []
        for line in f:
            if line.startswith('#'):
                continue
            fields = line.split()
            clk, ptr, chosen = int(fields[0]), int(fields[1]), int(fields[2])
            entries = []
            for e in fields[3:]:
                pos, flat, src, cmd, ready, opened, hit = e.split(',')
                entries.append((int(pos), int(flat), src, int(cmd), int(ready), int(opened), int(hit)))
            count['decisions'] += 1

            for pos, flat, src, cmd, ready, opened, hit in entries:
                if pos != (flat % bpg) * groups + flat // bpg:
                    bad['order'] += 1
                want = 'A' if opened else ('H' if hit else 'O')
                if src != want:
                    bad['head'] += 1

            walk = sorted(entries, key=lambda e: (e[0] - ptr) % n)
            ready = [e for e in walk if e[4]]
            expect = ready[0][0] if ready else -1
            if chosen != expect:
                bad['choice'] += 1
                if len(examples) < 5:
                    examples.append(f'clk {clk}: chose {chosen}, expected {expect} (pointer {ptr})')
            if prev_ptr_next is not None and ptr != prev_ptr_next:
                bad['pointer'] += 1
                if len(examples) < 5:
                    examples.append(f'clk {clk}: pointer {ptr}, expected {prev_ptr_next}')
            prev_ptr_next = (chosen + 1) % n if chosen >= 0 else ptr

            if chosen < 0:
                count['none ready'] += 1
            else:
                c = next(e for e in entries if e[0] == chosen)
                count[f'chosen head {c[2]}'] += 1
                # heads earlier in the walk than the chosen one (all not ready, by rule 3)
                count['not-ready heads passed'] += sum(1 for e in walk if (e[0] - ptr) % n < (chosen - ptr) % n)
    print(f'{path}: {count["decisions"]} decisions, none ready {count["none ready"]}; chosen heads: '
          f'opened {count["chosen head A"]}, row hit {count["chosen head H"]}, oldest {count["chosen head O"]}; '
          f'not-ready heads passed over {count["not-ready heads passed"]}')
    print('  mismatches: ' + ', '.join(f'{k} {bad[k]}' for k in ('order', 'head', 'choice', 'pointer')))
    for e in examples:
        print('  ' + e)
    ok &= count['decisions'] > 0 and not any(bad.values())
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
