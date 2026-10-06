"""Check that memsim ticks Ramulator2 at the DRAM's real speed.

usage: python3 SCRIPTS/check_clock.py <memsim output> [<memsim output> ...]

For each run it prints:
  - DRAM cycles / memsim cycles, which must equal the clock ratio printed at
    startup (DDR5-4800: 3 DRAM cycles per 5 memsim cycles = 0.6);
  - refreshes per channel vs run time / tREFI (3.9 us): refresh happens every
    3.9 us of real time, so these agree only if the DRAM clock is right;
  - the core's average delay per miss vs Ramulator2's read latency converted
    from DRAM cycles to memsim cycles (should be about equal).
"""
import re
import sys

MEMSIM_NS = 0.25
TREFI_NS = 3900.0

for path in sys.argv[1:]:
    txt = open(path).read()
    sys_c = int(re.search(r"SYS_CYCLES\s*:\s*(\d+)", txt).group(1))
    dram_c = [int(x) for x in re.findall(r"^\s+cycles:\s*(\d+)", txt, re.M)]
    refs = [int(x) for x in re.findall(r"num_maintenance_reqs_served:\s*(\d+)", txt)]
    lat = [float(x) for x in re.findall(r"avg_read_latency:\s*([\d.]+)", txt)]
    core = float(re.search(r"CORE_00_AVGDELAY\s*:\s*([\d.]+)", txt).group(1))
    clock = re.search(r"Clock: (\d+) DRAM cycle\(s\) per (\d+) memsim", txt)
    mem_r, fe_r = int(clock.group(1)), int(clock.group(2))
    warn = 'WARNING' in re.search(r"Clock:.*", txt).group(0)
    print(f"{path.split('/')[-1]}: clock {mem_r}:{fe_r}{'   <-- startup WARNING printed' if warn else ''}")
    print(f"   DRAM cycles / memsim cycles = {dram_c[0]}/{sys_c} = {dram_c[0] / sys_c:.4f} (expect {mem_r / fe_r:.4f})")
    print(f"   refreshes per channel = {refs[0]}; real-time expectation run_time/3.9us = {sys_c * MEMSIM_NS / TREFI_NS:.0f}")
    print(f"   core delay per miss = {core:.1f} memsim cycles; Ramulator2 read latency "
          f"{lat[0]:.1f} DRAM cycles x {fe_r}/{mem_r} = {lat[0] * fe_r / mem_r:.1f} memsim cycles")
