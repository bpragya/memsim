"""Check an AdaptiveOpen (or Open) run from Ramulator2 CmdTraceRecorder traces
(export the config with MEMSIM_CMD_TRACE=<path> MEMSIM_ROW_POLICY=adaptive).
usage: check_rowpolicy.py <trace dir> <memsim output>
Checks:
  1. after RDA/WRA, the next command to that bank is ACT (the row really closed)
  2. after a plain RD/WR (row kept open), how often the next column command to
     that bank hits the same row (it should, since another request needed it)
  3. trace counts agree with the row policy's own stats
"""
import csv, glob, re, sys
from collections import Counter

tdir, outp = sys.argv[1], sys.argv[2]
cnt = Counter()
bad_after_ap = 0
kept_open = kept_open_hit = 0
for path in sorted(glob.glob(tdir + '/*.ch*')):
    last = {}  # bank -> (command, row)
    with open(path) as f:
        for r in csv.DictReader(f):
            cmd = r['command']
            cnt[cmd] += 1
            if cmd in ('REFab', 'REFsb', 'RFMab', 'RFMsb', 'PREab'):
                continue
            bank = (r['Rank'], r['BankGroup'], r['Bank'])
            row = r['Row']
            prev = last.get(bank)
            if prev:
                pcmd, prow = prev
                if pcmd in ('RDA', 'WRA') and cmd != 'ACT':
                    bad_after_ap += 1
                if pcmd in ('RD', 'WR') and cmd in ('RD', 'WR', 'RDA', 'WRA'):
                    kept_open += 1
                    kept_open_hit += (row == prow)
            last[bank] = (cmd, row)

stats = Counter()
for line in open(outp):
    m = re.match(r'\s+(rows_kept_open|rows_closed_rda|rows_closed_wra|auto_precharge_not_ready|row_hits|row_misses|row_conflicts):\s*(\d+)', line)
    if m:
        stats[m.group(1)] += int(m.group(2))

col = cnt['RD'] + cnt['WR'] + cnt['RDA'] + cnt['WRA']
print(f"  commands: ACT={cnt['ACT']} PREpb={cnt['PREpb']} RD={cnt['RD']} WR={cnt['WR']} RDA={cnt['RDA']} WRA={cnt['WRA']}")
print(f"  row stats: hits={stats['row_hits']} misses={stats['row_misses']} conflicts={stats['row_conflicts']}")
print(f"  [1] RDA/WRA followed by a non-ACT command on the same bank: {bad_after_ap}  (must be 0)")
if kept_open:
    print(f"  [2] RD/WR left open, next column command to that bank: same row {kept_open_hit}/{kept_open} "
          f"({100*kept_open_hit/kept_open:.1f}%)")
if stats['rows_closed_rda'] or stats['rows_kept_open']:
    pol = stats['rows_kept_open'] + stats['rows_closed_rda'] + stats['rows_closed_wra'] + stats['auto_precharge_not_ready']
    print(f"  [3] policy stats: kept_open={stats['rows_kept_open']} closed_rda={stats['rows_closed_rda']} "
          f"closed_wra={stats['rows_closed_wra']} ap_not_ready={stats['auto_precharge_not_ready']}")
    print(f"      RDA in trace = closed_rda? {cnt['RDA'] == stats['rows_closed_rda']}   "
          f"WRA in trace = closed_wra? {cnt['WRA'] == stats['rows_closed_wra']}   "
          f"policy decisions = column commands? {pol == col}")
