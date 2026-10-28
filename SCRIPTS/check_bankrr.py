"""Check the BankRR controller's write episodes, independently of its code.

usage: python3 SCRIPTS/check_bankrr.py <rr_log>.ch0 [<rr_log>.ch1 ...]

The log (Ramulator2 controller param rr_log, one file per sub-channel) has, per episode:
    E <clk> <bank>:<count>...          episode start: snapshot writes per bank
    P <clk> <phase> <round> <bank>...  phase start and its banks
    D <clk> <pointer> <chosen pos> {pos,bank,head_src,command,ready,opened,row_hit}...
    W <clk> <bank> <due>               a write of the episode was issued
    X <clk> <write queue size>         episode end
From each episode's snapshot alone, this script works out the phases that must follow and
checks the rules:
  1. phases: A = banks with >= 2 writes, B = banks with 1, C = banks with >= 2 (second
     write), then C again for banks with >= r writes (r = 3, 4, ...) while at least
     repeat_min_banks banks take part; empty phases are skipped.
  2. strict phases: a phase starts only when every bank of the previous one has written;
     each phase bank writes exactly once; no other bank writes.
  3. order: rotation position p is bank (p mod G) * B + p div G (B banks per group, G groups).
  4. head: a bank offers its opened write if any, else a row hit if one is queued, else its
     oldest write.
  5. choice: from the pointer, the first phase bank whose head is ready (none -> -1); the
     pointer then moves past it.
  6. the episode ends right after its last phase.
"""
import sys
from collections import Counter


def expected_phases(counts, threshold):
    heavy = {b for b, c in counts.items() if c >= 2}
    light = {b for b, c in counts.items() if c == 1}
    phases = []
    if heavy:
        phases.append(('A', 1, heavy))
    if light:
        phases.append(('B', 1, light))
    if heavy:
        phases.append(('C', 2, heavy))
    r = 3
    while True:
        banks = {b for b, c in counts.items() if c >= r}
        if not banks or len(banks) < threshold:
            break
        phases.append(('C', r, banks))
        r += 1
    return phases


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
        phases, idx, todo = [], -1, set()
        ptr_next = None
        for line in f:
            if line.startswith('#'):
                continue
            f_ = line.split()
            kind, clk = f_[0], int(f_[1])
            if kind == 'E':
                if in_episode:
                    fail('episode', f'clk {clk}: new episode before the previous one ended')
                counts = {int(b): int(c) for b, c in (x.split(':') for x in f_[2:])}
                phases, idx, todo, in_episode = expected_phases(counts, threshold), -1, set(), True
                count['episodes'] += 1
                count['snapshot writes'] += sum(counts.values())
            elif kind == 'P':
                phase, rnd, banks = f_[2], int(f_[3]), {int(b) for b in f_[4:]}
                if todo:
                    fail('strict', f'clk {clk}: phase {phase}{rnd} started with {len(todo)} banks of the last phase unwritten')
                idx += 1
                if idx >= len(phases) or phases[idx] != (phase, rnd, banks):
                    want = phases[idx][:2] if idx < len(phases) else 'episode end'
                    fail('phases', f'clk {clk}: phase {phase}{rnd} with {len(banks)} banks, expected {want}')
                todo = set(banks)
                count[f'phase {phase}' if rnd <= 2 else 'phase C repeat'] += 1
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
            elif kind == 'W':
                bank, due = int(f_[2]), int(f_[3])
                if not due or bank not in todo:
                    fail('writes', f'clk {clk}: bank {bank} wrote out of turn (due {due})')
                todo.discard(bank)
                count['writes'] += 1
            elif kind == 'X':
                if todo or idx != len(phases) - 1:
                    fail('end', f'clk {clk}: episode ended after phase {idx + 1} of {len(phases)}, {len(todo)} banks unwritten')
                in_episode = False
                count['queue at end'] += int(f_[2])
    complete = count['episodes'] - int(in_episode)
    print(f'{path}: {count["episodes"]} episodes ({complete} complete), {count["snapshot writes"]} snapshot writes, '
          f'{count["writes"]} issued; phases A {count["phase A"]}, B {count["phase B"]}, C {count["phase C"]}, '
          f'C repeats {count["phase C repeat"]}; {count["decisions"]} decisions, none ready {count["none ready"]}; '
          f'mean write queue at episode end {count["queue at end"] / max(complete, 1):.1f}')
    print('  mismatches: ' + ', '.join(f'{k} {bad[k]}' for k in
                                       ('episode', 'phases', 'strict', 'order', 'head', 'choice', 'pointer', 'writes', 'end')))
    for e in examples:
        print('  ' + e)
    ok &= count['episodes'] > 0 and not any(bad.values())
print('PASS' if ok else 'FAIL')
sys.exit(0 if ok else 1)
