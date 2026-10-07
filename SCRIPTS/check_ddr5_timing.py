"""Check a run's DRAM command trace against BARD Table I (DDR5-4800B, x4 chips).

Export the config with MEMSIM_CMD_TRACE=<trace> (one CSV per channel,
<trace>.ch0/.ch1), run memsim, then:
    python3 SCRIPTS/check_ddr5_timing.py <trace>

For every rule below it finds every pair of commands the rule applies to and
reports the smallest gap actually observed (in DRAM cycles, tCK = 0.416 ns);
the run passes if no gap is below Table I. "At limit" counts pairs exactly at
the minimum, i.e. where the rule was what held the command back.

DDR5 ACT, RD, WR, RDA and WRA take two command-bus cycles. Ramulator2 logs a
command at its first cycle, but the DRAM sees it at its last, so gaps are
measured last cycle to last cycle (the same adjustment Ramulator2 applies to
its own constraints): a PRE logged at t and an ACT logged at t+38 are 39
cycles apart.
"""
import csv
import glob
import sys

# Table I (cycles): CL 40, CWL 38, tRCD 39, tRP 39, tRAS 77, tWR 72, BL/2 8,
# tCCD_S_WR 8, tCCD_L_WR 48. nRTP (18) is from the same DDR5-4800B preset.
CWL, TRCD, TRP, TRAS, TWR, BL2, TCCD_S_WR, TCCD_L_WR, NRTP = 38, 39, 39, 77, 72, 8, 8, 48, 18
RULES = [
    # name,                                   table I terms,                    required
    ('ACT -> RD/WR, same bank',              'tRCD',                           TRCD),
    ('PRE -> ACT, same bank',                'tRP',                            TRP),
    ('ACT -> PRE, same bank',                'tRAS',                           TRAS),
    ('WR -> PRE, same bank',                 'CWL + BL/2 + tWR',               CWL + BL2 + TWR),
    ('RDA -> ACT, same bank',                'nRTP + tRP',                     NRTP + TRP),
    ('WRA -> ACT, same bank',                'CWL + BL/2 + tWR + tRP',         CWL + BL2 + TWR + TRP),
    ('RD -> RD, any bank (data bus)',        'BL/2',                           BL2),
    ('WR -> WR, different bank group',       'tCCD_S_WR',                      TCCD_S_WR),
    ('WR -> WR, same bank group',            'tCCD_L_WR',                      TCCD_L_WR),
]
READS, WRITES = ('RD', 'RDA'), ('WR', 'WRA')
CYCLES = {'ACT': 2, 'RD': 2, 'WR': 2, 'RDA': 2, 'WRA': 2}  # others take 1


def last_cycle(clock, cmd):
    """Clock of a command's last command-bus cycle (the one the DRAM acts on)."""
    return clock + CYCLES.get(cmd, 1) - 1

paths = sorted(glob.glob(sys.argv[1] + '.ch*'))
if not paths:
    sys.exit(f'no command traces {sys.argv[1]}.ch*')

seen = {name: [None, 0, 0] for name, _, _ in RULES}  # min gap, pairs, pairs at the minimum


def note(rule, gap):
    s = seen[rule]
    s[1] += 1
    if s[0] is None or gap < s[0]:
        s[0], s[2] = gap, 1
    elif gap == s[0]:
        s[2] += 1


for path in paths:
    last_act, last_pre, last_wr, last_ap = {}, {}, {}, {}  # per bank
    last_rd_any = last_wr_any = None
    last_wr_bg, last_wr_bg_bank = {}, {}                     # per bank group
    with open(path) as f:
        for r in csv.DictReader(f):
            cmd = r['command']
            c = last_cycle(int(r['clock']), cmd)
            rank, bg = r['Rank'], r['BankGroup']
            bank = (rank, bg, r['Bank'])
            if cmd == 'ACT':
                if bank in last_pre:
                    note('PRE -> ACT, same bank', c - last_pre[bank])
                if bank in last_ap:
                    kind, t = last_ap.pop(bank)
                    note(f'{kind} -> ACT, same bank', c - t)
                last_act[bank] = c
            elif cmd in ('PREpb', 'PREab'):
                banks = [bank] if cmd == 'PREpb' else [b for b in set(last_act) | set(last_wr) if b[0] == rank]
                for b in banks:
                    if b in last_act:
                        note('ACT -> PRE, same bank', c - last_act[b])
                    if b in last_wr:
                        note('WR -> PRE, same bank', c - last_wr.pop(b))
                    last_pre[b] = c
                    last_act.pop(b, None)
            elif cmd in READS + WRITES:
                if bank in last_act:
                    note('ACT -> RD/WR, same bank', c - last_act[bank])
                if cmd in READS:
                    if last_rd_any is not None:
                        note('RD -> RD, any bank (data bus)', c - last_rd_any)
                    last_rd_any = c
                else:
                    if last_wr_any is not None and last_wr_any[1] != bg:
                        note('WR -> WR, different bank group', c - last_wr_any[0])
                    if bg in last_wr_bg:
                        note('WR -> WR, same bank group', c - last_wr_bg[bg])
                    last_wr_any = (c, bg)
                    last_wr_bg[bg] = c
                    last_wr[bank] = c
                if cmd in ('RDA', 'WRA'):            # auto-precharge closes the row
                    last_ap[bank] = (cmd, c)
                    last_act.pop(bank, None)
                    last_wr.pop(bank, None)
                    last_pre.pop(bank, None)

ok = True
print(f"{'rule':34} {'Table I':24} {'need':>5} {'min seen':>9} {'pairs':>9} {'at limit':>9}")
for name, terms, need in RULES:
    mn, n, at = seen[name]
    good = mn is not None and mn >= need
    ok &= good
    print(f"{name:34} {terms:24} {need:5} {('-' if mn is None else mn):>9} {n:9} {at:9}  {'ok' if good else 'VIOLATION' if mn is not None else 'no pairs'}")
print('PASS' if ok else 'FAIL')
