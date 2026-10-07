"""Check Ramulator2's write-drain statistics against its own command trace.

Export the config with MEMSIM_CMD_TRACE=<trace> and MEMSIM_DRAIN_LOG=<log>,
run memsim, then:
    python3 SCRIPTS/check_write_stats.py <trace> <log> <memsim output>

A drain owns the writes it picks from the write buffer while in write mode;
a picked write's WR may issue a little after write mode ends. From
<trace>.chN alone, a write is "picked" at its ACT (trace type 1 = write) or,
for a row hit with no ACT, at its WR. For every drain in <log>.chN (using
only its start, mode_end and close clocks) this recomputes the writes it
owns, the distinct banks written (write BLP) and the WR-to-WR gaps, and
compares them with what the controller logged. It then checks that the
per-channel totals match the printed stats, that no drain writes more banks
than a channel has, and reports the smallest WR-to-WR gaps seen (Table I:
8 cycles across bank groups, 48 within a bank group).
"""
import bisect
import csv
import glob
import re
import sys

trace, log, outp = sys.argv[1], sys.argv[2], sys.argv[3]

# per-channel stats printed by the controllers, in order (Channel 0, Channel 1)
printed = []
for line in open(outp):
    m = re.match(r'\s+(write_drains|drain_writes|drain_banks_total|drain_wr2wr_gaps|drain_wr2wr_total):\s*(\d+)', line)
    if m:
        if m.group(1) == 'write_drains':
            printed.append({})
        printed[-1][m.group(1)] = int(m.group(2))

ok = True
for ch, logpath in enumerate(sorted(glob.glob(log + '.ch*'))):
    wr = []      # (clock, bank) of every WR/WRA on this channel
    picked = []  # when each of those writes left the write buffer
    banks_per_channel = set()
    write_act = {}  # bank -> clock of the ACT opened for a write, until its WR
    with open(f'{trace}.ch{ch}') as f:
        for r in csv.DictReader(f):
            bank = (r['Rank'], r['BankGroup'], r['Bank'])
            if r['command'] == 'ACT':
                banks_per_channel.add(bank)
                if r['type'] == '1':
                    write_act[bank] = int(r['clock'])
            elif r['command'] in ('WR', 'WRA'):
                c = int(r['clock'])
                wr.append((c, bank))
                picked.append(write_act.pop(bank, c))
    clocks = [c for c, _ in wr]

    drains = mismatched = 0
    tot = dict(drain_writes=0, drain_banks_total=0, drain_wr2wr_gaps=0, drain_wr2wr_total=0)
    max_blp = 0
    min_writes = None  # watermarks guarantee >= high - low writes per drain
    # A "# stats reset" line (end of warmup) restarts the totals that are
    # compared with the printed stats; every drain is still checked.
    log_lines = open(logpath).read().splitlines()
    header = log_lines[0]
    for line in log_lines[1:]:
        if line.startswith('#'):
            drains = 0
            tot = dict.fromkeys(tot, 0)
            max_blp = 0
            min_writes = None
            continue
        d = dict(zip(header.split(','), line.split(',')))
        start, mode_end, close = int(d['start']), int(d['mode_end']), int(d['close'])
        lo, hi = bisect.bisect_left(clocks, start), bisect.bisect_right(clocks, close)
        window = [wr[i] for i in range(lo, hi) if start <= picked[i] <= mode_end]
        writes = len(window)
        banks = len({b for _, b in window})
        gaps = [window[i][0] - window[i - 1][0] for i in range(1, len(window))]
        logged = (int(d['writes']), int(d['banks']), int(d['wr2wr_total']), int(d['wr2wr_gaps']))
        if (writes, banks, sum(gaps), len(gaps)) != logged:
            mismatched += 1
            if mismatched <= 3:
                print(f'  ch{ch} drain @{start}: trace (writes,banks,gap_total,gaps)='
                      f'{(writes, banks, sum(gaps), len(gaps))} log={logged}')
        if d['counted'] == '1':
            drains += 1
            tot['drain_writes'] += logged[0]
            tot['drain_banks_total'] += logged[1]
            tot['drain_wr2wr_total'] += logged[2]
            tot['drain_wr2wr_gaps'] += logged[3]
            max_blp = max(max_blp, logged[1])
            min_writes = logged[0] if min_writes is None else min(min_writes, logged[0])

    # smallest WR-to-WR gaps on the whole channel
    min_any = min_same_bg = None
    last_bg = {}
    for i, (c, b) in enumerate(wr):
        if i:
            g = c - wr[i - 1][0]
            min_any = g if min_any is None else min(min_any, g)
        bg = b[:2]
        if bg in last_bg:
            g = c - last_bg[bg]
            min_same_bg = g if min_same_bg is None else min(min_same_bg, g)
        last_bg[bg] = c

    p = printed[ch] if ch < len(printed) else {}
    totals_ok = (p.get('write_drains') == drains and all(p.get(k) == v for k, v in tot.items()))
    ok &= (mismatched == 0) and totals_ok and max_blp <= len(banks_per_channel)
    blp = tot['drain_banks_total'] / drains if drains else 0
    w2w = tot['drain_wr2wr_total'] / tot['drain_wr2wr_gaps'] if tot['drain_wr2wr_gaps'] else 0
    print(f'ch{ch}: {drains} drains, avg BLP {blp:.2f} (max {max_blp} of {len(banks_per_channel)} banks), '
          f'avg WR-to-WR {w2w:.2f} cycles')
    print(f'  fewest writes in a drain: {min_writes} (at least high - low watermark)')
    print(f'  drains whose trace window disagrees with the log: {mismatched}')
    print(f'  log totals match printed stats: {totals_ok}')
    print(f'  smallest WR-to-WR gap: any bank {min_any}, same bank group {min_same_bg}')

print('PASS' if ok else 'FAIL')
