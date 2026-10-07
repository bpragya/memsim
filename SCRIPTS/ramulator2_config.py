"""
Ramulator2 config generator for memsim.

Builds a Ramulator2 config matching memsim's DRAM geometry
(2 channels x 32 banks = 64 total, per src/params.h MEM_CHANNELS/MEM_BANKS)
and exposes memsim's refresh/RFM/scheduler knobs as CLI flags so a
matching YAML can be regenerated for a given simulator run.

DDR5 is the only Ramulator2-supported standard with RFM commands
(RFMab/RFMsb) -- appropriate here since RFM is what this project studies.
org_preset "DDR5_16Gb_x8" gives bankgroup=8 x bank=4 = 32 banks/channel
at rank=1, matching MEM_BANKS/MEM_CHANNELS exactly.

Clocks: memsim models a 4 GHz core (one cycle = 0.25 ns). The frontend and
memory-system clock_ratio values are derived from the DRAM preset's tCK so
that src/memsys.c ticks Ramulator2 at the DRAM's real speed: it calls
memory_system->tick() memory_ratio/frontend_ratio times per memsim cycle.
DDR5_8000AN (tCK 250 ps) gives 1:1; DDR5_4800B (tCK 416 ps) gives 5:3, i.e.
3 DRAM cycles per 5 memsim cycles.

Usage:
  python -m ramulator export SCRIPTS/ramulator2_config.py -o src/ramulator2_config.yaml

  `python -m ramulator export` does not forward extra CLI args to the
  target script, so settings are read from environment variables instead:

    MEMSIM_REF_MODE={ab,sb}              default ab   (matches -dramrefpolicy 2/1)
    MEMSIM_RFM_MODE={off,ab,sb}          default off  (matches -dramrfmpolicy 0/2/1)
    MEMSIM_RFM_THRESH=<int>              default 16   (matches -rfmth)
    MEMSIM_SCHED={frfcfs,frfcfs_rowhit}  default frfcfs (matches -dramschedpolicy)
    MEMSIM_CLOSEPAGE={0,1}               default 0    (matches -memclosepage)
    MEMSIM_ROW_POLICY={open,adaptive,closedcap}
                                         default open (closedcap if MEMSIM_CLOSEPAGE=1);
                                         adaptive = adaptive open-page (BARD Table II)
    MEMSIM_CMD_TRACE=<path>              default off: record every DRAM command to
                                         <path>.ch0/.ch1 (for SCRIPTS/check_rowpolicy.py);
                                         use a Linux path, and MSYS_NO_PATHCONV=1 in Git Bash
    MEMSIM_DRAIN_LOG=<path>              default off: one line per write drain to
                                         <path>.ch0/.ch1 (for SCRIPTS/check_write_stats.py)
    MEMSIM_ADDR_MAP={mop4clxor,robaracoch,zen_pbpl,zen} default mop4clxor
                                         (mop4clxor/robaracoch match -drammappolicy 7/6;
                                          zen_pbpl = AMD Zen + PBPL, zen = without PBPL)
    MEMSIM_CONFIG={default,table2}       default: DDR5-8000AN x8, 32/32 queues, MOP4CLXOR, open page.
                                         table2: BARD's baseline (Table I/II): DDR5-4800B x4
                                         chips (32 GB), Zen + PBPL, adaptive open-page, 64-entry
                                         read queue, write queue drained from size-8 down to 8.
                                         The options above still override it.
    MEMSIM_WRQ_SIZE=<int>                table2 write-queue entries, default 48 (watermarks
                                         high = size-8, low = 8: drain at 40, stop at 8)

  Table II also needs memsim flags; see SCRIPTS/run_bard.sh:
    -ratemode 8 -l3sizemb 16 -l3assoc 16 -robsize 512 -llcmshrs 128
    -warmup 25000000 -inst_limit 100000000

  Example:
    MEMSIM_RFM_MODE=sb MEMSIM_RFM_THRESH=16 \\
      python -m ramulator export SCRIPTS/ramulator2_config.py -o src/ramulator2_config_rfm.yaml
"""

import os
from fractions import Fraction

import ramulator


class Args:
    config = os.environ.get("MEMSIM_CONFIG", "default")
    table2 = config == "table2"
    if config not in ("default", "table2"):
        raise ValueError(f"MEMSIM_CONFIG must be default or table2, not {config}")
    wrq_size = int(os.environ.get("MEMSIM_WRQ_SIZE", "48"))
    ref_mode = os.environ.get("MEMSIM_REF_MODE", "ab")
    rfm_mode = os.environ.get("MEMSIM_RFM_MODE", "off")
    rfm_thresh = int(os.environ.get("MEMSIM_RFM_THRESH", "16"))
    cmd_trace = os.environ.get("MEMSIM_CMD_TRACE", "")
    drain_log = os.environ.get("MEMSIM_DRAIN_LOG", "")
    sched = os.environ.get("MEMSIM_SCHED", "frfcfs")
    closepage = os.environ.get("MEMSIM_CLOSEPAGE", "0") == "1"
    row_policy = os.environ.get("MEMSIM_ROW_POLICY",
                                "closedcap" if closepage else ("adaptive" if table2 else "open"))
    addr_map = os.environ.get("MEMSIM_ADDR_MAP", "zen_pbpl" if table2 else "mop4clxor")


args = Args()


def make_addr_mapper():
    if args.addr_map == "robaracoch":
        return ramulator.addr_mapper.RoBaRaCoCh()
    if args.addr_map == "zen_pbpl":
        return ramulator.addr_mapper.ZenPBPL()            # AMD Zen + PBPL (BARD Fig. 6)
    if args.addr_map == "zen":
        return ramulator.addr_mapper.ZenPBPL(pbpl=False)  # AMD Zen without PBPL
    return ramulator.addr_mapper.MOP4CLXOR()


MEMSIM_CYCLE_PS = 250  # memsim models a 4 GHz core: one cycle = 0.25 ns


def make_dram():
    if args.table2:
        # BARD Table I: DDR5-4800B, x4 chips (on-die ECC read-modify-write
        # makes same-bank-group writes 48 cycles apart); 2 x 16 GB
        return ramulator.dram.DDR5(org_preset="DDR5_16Gb_x4", timing_preset="DDR5_4800B", rank=1)
    return ramulator.dram.DDR5(
        org_preset="DDR5_16Gb_x8",
        timing_preset="DDR5_8000AN",
        rank=1,
    )


def queue_settings():
    """Table II: 64-entry read queue; N-entry write queue drained from N-8 down to 8.

    Ramulator2 enters write mode when occupancy > high*N and leaves when
    occupancy < low*N, so the fractions sit half an entry inside the
    thresholds: drains start at exactly N-8 and stop at exactly 8.
    """
    if not args.table2:
        return {}
    n = args.wrq_size
    return {
        "read_buffer_size": 64,
        "write_buffer_size": n,
        "wr_high_watermark": (n - 8 - 0.5) / n,
        "wr_low_watermark": 8.5 / n,
    }


def clock_ratios():
    """(frontend, memory_system) clock_ratio for the DRAM preset's tCK.

    memsim ticks the DRAM memory/frontend times per memsim cycle, so the
    ratio frontend/memory must equal tCK / memsim cycle (416/250 -> 5/3).
    """
    _, timing = make_dram().resolve()
    ratio = Fraction(timing["tCK_ps"], MEMSIM_CYCLE_PS).limit_denominator(16)
    return ratio.numerator, ratio.denominator


def make_controller():
    dram = make_dram()

    scheduler = (
        ramulator.scheduler.FRFCFS_RowHit()
        if args.sched == "frfcfs_rowhit"
        else ramulator.scheduler.FRFCFS()
    )

    refresh_manager = ramulator.refresh_manager.AllBank()  # AB scope is standard-wide default; SB refresh via REFsb is not yet exposed as a stock refresh_manager

    if args.row_policy == "adaptive":
        row_policy = ramulator.row_policy.AdaptiveOpen()  # close a row when no queued request needs it
    elif args.row_policy == "closedcap":
        row_policy = ramulator.row_policy.ClosedCAP()
    else:
        row_policy = ramulator.row_policy.Open()

    plugins = []
    if args.cmd_trace:
        # one CSV per channel: <path>.ch0, <path>.ch1 (clock, command, address vector)
        plugins.append(ramulator.controller_plugin.CmdTraceRecorder(path=args.cmd_trace))
    if args.rfm_mode != "off":
        plugins.append(
            ramulator.controller_plugin.RFMManager(
                rfm_thresh=args.rfm_thresh,
                rfm_mode=args.rfm_mode,
            )
        )

    extra = queue_settings()
    if args.drain_log:
        extra["drain_log"] = args.drain_log  # one CSV line per write drain, per channel

    return ramulator.controller.GenericDDR(
        dram=dram,
        scheduler=scheduler,
        refresh_manager=refresh_manager,
        row_policy=row_policy,
        addr_mapper=make_addr_mapper(),
        controller_plugins=plugins,
        **extra,
    )


# 2 channels, matching memsim's default MEM_CHANNELS=2 / MEM_BANKS=64
frontend_ratio, memory_ratio = clock_ratios()
frontend = ramulator.frontend.External(clock_ratio=frontend_ratio)

mem = ramulator.memory_system.GenericDRAM(
    clock_ratio=memory_ratio,
    controllers=[make_controller(), make_controller()],
    channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
)

sim = ramulator.Simulation(frontend, mem)
