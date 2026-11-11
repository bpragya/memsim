"""Check the BankRR controller's write episodes, independently of its code.

usage: python3 SCRIPTS/check_bankrr.py <rr_log>.ch0 [<rr_log>.ch1 ...]

The log (Ramulator2 controller param rr_log, one file per sub-channel) has, per episode:
    E <clk> <bank>:<count>...          episode start: snapshot writes per bank
    P <clk> <phase> <round> <bank>...  phase start and its banks
    D <clk> <pointer> <chosen pos> {pos,bank,head_src,command,ready,opened,row_hit}...
    O <clk> <pointer> <chosen pos> {pos,bank,kind,ready}...   phase A: prepare a phase-B write
    W <clk> <bank> <due>               a write was issued (1 = its turn, 2 = phase-B write
                                       forced out during A by a refresh)
    X <clk> <write queue size>         episode end
From each episode's snapshot alone, this script works out the phases that must follow and
checks the rules:
  1. phases: A = banks with >= 2 writes, B = banks with 1 (minus any whose write a refresh
     forced out during A), C = banks with >= 2 (second write), then C again for banks with
     >= r writes (r = 3, 4, ...) while at least repeat_min_banks banks take part; empty
     phases are skipped.
  2. strict phases: a phase starts only when every bank of the previous one has written;
     each phase bank writes exactly once; no other bank writes, except a phase-B bank
     during A when a refresh forces it (W ... 2).
  3. order: rotation position p is bank (p mod G) * B + p div G (B banks per group, G groups).
  4. head: a bank offers its opened write if any, else a row hit if one is queued, else its
     oldest write.
  5. choice: from the pointer, the first phase bank whose head is ready (none -> -1); the
     pointer then moves past it.
  6. phase-B rows opened during A: only in phase A, only in a cycle where no A bank was
     ready (a D line choosing -1 at the same clk), only for phase-B banks, and the first
     bank from the pointer whose next command is a PRE/ACT that may issue; the pointer does
     not move.
  7. the episode ends right after its last phase.
"""
import sys
from collections import Counter

ok = True
for path in sys.argv[1:]:
    bad = Counter()
    count = Counter()
    examples = []

    def fail(kind, msg):
        bad[kind] += 1
        if len(examples) < 6:
            examples.append(msg)

    with open(path) as f:
        head = f.readline().split()   # # bankrr-episodes banks <n> banks_per_group <b> repeat_min_banks <t>
        hdr = dict(zip(head[2::2], head[3::2]))
        n, bpg, threshold = int(hdr['banks']), int(hdr['banks_per_group']), int(hdr['repeat_min_banks'])
        groups = n // bpg
        in_episode = False
        counts, forced = {}, set()
        phase, todo = None, set()
        ptr_next = None
        last_d = None   # (clk, chosen) of the latest D line

        def next_phase(prev):
            """The phase that must follow prev (None = episode start), or None = episode end."""
            heavy = {b for b, c in counts.items() if c >= 2}
            light = {b for b, c in counts.items() if c == 1} - forced
            if prev is None and heavy:
                return ('A', 1, heavy)
            if prev is None or prev[0] == 'A':
                if light:
                    return ('B', 1, light)
                return ('C', 2, heavy) if heavy else None
            if prev[0] == 'B':
                return ('C', 2, heavy) if heavy else None
            r = prev[1] + 1
            banks = {b for b, c in counts.items() if c >= r}
            return ('C', r, banks) if banks and len(banks) >= threshold else None

        for line in f:
            if line.startswith('#'):
                continue
            f_ = line.split()
            kind, clk = f_[0], int(f_[1])
            if kind == 'E':
                if in_episode:
                    fail('episode', f'clk {clk}: new episode before the previous one ended')
                counts = {int(b): int(c) for b, c in (x.split(':') for x in f_[2:])}
                forced, phase, todo, in_episode = set(), None, set(), True
                count['episodes'] += 1
                count['snapshot writes'] += sum(counts.values())
            elif kind == 'P':
                got = (f_[2], int(f_[3]), {int(b) for b in f_[4:]})
                if todo:
                    fail('strict', f'clk {clk}: phase {got[0]}{got[1]} started with {len(todo)} banks of the last phase unwritten')
                want = next_phase(phase)
                if got != want:
                    fail('phases', f'clk {clk}: phase {got[0]}{got[1]} with {len(got[2])} banks, '
                                   f'expected {want[:2] if want else "episode end"}')
                phase, todo = got[:2], set(got[2])
                count[f'phase {got[0]}' if got[1] <= 2 else 'phase C repeat'] += 1
            elif kind == 'D':
                ptr, chosen = int(f_[2]), int(f_[3])
                entries = []
                for e in f_[4:]:
                    pos, bank, src, cmd, ready, opened, hit = e.split(',')
                    entries.append((int(pos), int(bank), src, int(ready), int(opened), int(hit)))
                count['decisions'] += 1
                if {e[1] for e in entries} != todo:
                    fail('strict', f'clk {clk}: decision offered banks other than the phase\'s remaining banks')
                for pos, bank, src, ready, opened, hit in entries:
                    if pos != (bank % bpg) * groups + bank // bpg:
                        fail('order', f'clk {clk}: bank {bank} at position {pos}')
                    if src != ('A' if opened else ('H' if hit else 'O')):
                        fail('head', f'clk {clk}: bank {bank} offered head {src}')
                walk = sorted(entries, key=lambda e: (e[0] - ptr) % n)
                ready = [e for e in walk if e[3]]
                expect = ready[0][0] if ready else -1
                if chosen != expect:
                    fail('choice', f'clk {clk}: chose position {chosen}, expected {expect} (pointer {ptr})')
                if ptr_next is not None and ptr != ptr_next:
                    fail('pointer', f'clk {clk}: pointer {ptr}, expected {ptr_next}')
                ptr_next = (chosen + 1) % n if chosen >= 0 else ptr
                count['none ready'] += chosen < 0
                last_d = (clk, chosen)
            elif kind == 'O':
                ptr, chosen = int(f_[2]), int(f_[3])
                entries = [(int(p), int(b), k, int(r)) for p, b, k, r in (e.split(',') for e in f_[4:])]
                count['B-row decisions'] += 1
                light = {b for b, c in counts.items() if c == 1}
                if not phase or phase[0] != 'A' or last_d != (clk, -1):
                    fail('prep', f'clk {clk}: phase-B row step outside phase A or while an A bank was ready')
                if not {e[1] for e in entries} <= light - forced:
                    fail('prep', f'clk {clk}: row step offered banks that are not phase-B banks')
                for pos, bank, k, r in entries:
                    if pos != (bank % bpg) * groups + bank // bpg:
                        fail('order', f'clk {clk}: bank {bank} at position {pos}')
                    if r and k != 'P':
                        fail('prep', f'clk {clk}: bank {bank} ready for a row step but its next command is the WR')
                walk = sorted(entries, key=lambda e: (e[0] - ptr) % n)
                ready = [e for e in walk if e[2] == 'P' and e[3]]
                expect = ready[0][0] if ready else -1
                if chosen != expect:
                    fail('prep', f'clk {clk}: row step chose position {chosen}, expected {expect} (pointer {ptr})')
                count['B rows prepared'] += chosen >= 0
            elif kind == 'W':
                bank, due = int(f_[2]), int(f_[3])
                if due == 2:
                    if not phase or phase[0] != 'A' or counts.get(bank) != 1 or bank in forced:
                        fail('writes', f'clk {clk}: bank {bank} marked refresh-forced outside the rules')
                    forced.add(bank)
                    count['forced B writes'] += 1
                elif not due or bank not in todo:
                    fail('writes', f'clk {clk}: bank {bank} wrote out of turn (due {due})')
                todo.discard(bank)
                count['writes'] += 1
            elif kind == 'X':
                if todo or next_phase(phase) is not None:
                    fail('end', f'clk {clk}: episode ended with {len(todo)} banks unwritten or a phase still due')
                in_episode = False
                count['queue at end'] += int(f_[2])
    complete = count['episodes'] - int(in_episode)
    print(f'{path}: {count["episodes"]} episodes ({complete} complete), {count["snapshot writes"]} snapshot writes, '
          f'{count["writes"]} issued; phases A {count["phase A"]}, B {count["phase B"]}, C {count["phase C"]}, '
          f'C repeats {count["phase C repeat"]}; {count["decisions"]} decisions, none ready {count["none ready"]}; '
          f'phase-B rows prepared during A {count["B rows prepared"]} (forced B writes {count["forced B writes"]}); '
          f'mean write queue at episode end {count["queue at end"] / max(complete, 1):.1f}')
    print('  mismatches: ' + ', '.join(f'{k} {bad[k]}' for k in
                                       ('episode', 'phases', 'strict', 'order', 'head', 'choice', 'pointer', 'prep',
                                        'writes', 'end')))
    for e in examples:
        print('  ' + e)
    ok &= count['episodes'] > 0 and not any(bad.values())
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
