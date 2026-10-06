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

NOTE on timing fidelity: timing_preset="DDR5_8000AN" is chosen specifically
because its tCK_ps=250 matches memsim's assumed 4GHz/0.25ns-per-cycle clock
exactly (src/memsys.c calls memory_system->tick() once per memsim cycle,
with no conversion factor -- so the two clocks must line up, or DRAM
operations resolve in the wrong number of memsim cycles). This does NOT
make every individual nCL/nRCD/nRAS/... constant bit-identical to memsim's
original hand-tuned tRC/tCAS/tRAS/... values from params.h (those are
JEDEC-standard DDR5-8000 timings, not memsim's own approximated numbers),
and row-buffer size / address-mapping scheme are still stock choices, not
calibrated to memsim's original custom model. Those remain follow-on
calibration work -- this config removes the systematic clock-domain
distortion and gets latencies into the correct real-ns ballpark.

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
    MEMSIM_ADDR_MAP={mop4clxor,robaracoch,zen_pbpl,zen} default mop4clxor
                                         (mop4clxor/robaracoch match -drammappolicy 7/6;
                                          zen_pbpl = AMD Zen + PBPL, zen = without PBPL)

  Example:
    MEMSIM_RFM_MODE=sb MEMSIM_RFM_THRESH=16 \\
      python -m ramulator export SCRIPTS/ramulator2_config.py -o src/ramulator2_config_rfm.yaml
"""

import os

import ramulator


class Args:
    ref_mode = os.environ.get("MEMSIM_REF_MODE", "ab")
    rfm_mode = os.environ.get("MEMSIM_RFM_MODE", "off")
    rfm_thresh = int(os.environ.get("MEMSIM_RFM_THRESH", "16"))
    cmd_trace = os.environ.get("MEMSIM_CMD_TRACE", "")
    sched = os.environ.get("MEMSIM_SCHED", "frfcfs")
    closepage = os.environ.get("MEMSIM_CLOSEPAGE", "0") == "1"
    row_policy = os.environ.get("MEMSIM_ROW_POLICY", "closedcap" if closepage else "open")
    addr_map = os.environ.get("MEMSIM_ADDR_MAP", "mop4clxor")


args = Args()


def make_addr_mapper():
    if args.addr_map == "robaracoch":
        return ramulator.addr_mapper.RoBaRaCoCh()
    if args.addr_map == "zen_pbpl":
        return ramulator.addr_mapper.ZenPBPL()            # AMD Zen + PBPL (BARD Fig. 6)
    if args.addr_map == "zen":
        return ramulator.addr_mapper.ZenPBPL(pbpl=False)  # AMD Zen without PBPL
    return ramulator.addr_mapper.MOP4CLXOR()


def make_controller():
    dram = ramulator.dram.DDR5(
        org_preset="DDR5_16Gb_x8",
        timing_preset="DDR5_8000AN",
        rank=1,
    )

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

    return ramulator.controller.GenericDDR(
        dram=dram,
        scheduler=scheduler,
        refresh_manager=refresh_manager,
        row_policy=row_policy,
        addr_mapper=make_addr_mapper(),
        controller_plugins=plugins,
    )


# 2 channels, matching memsim's default MEM_CHANNELS=2 / MEM_BANKS=64
frontend = ramulator.frontend.External(clock_ratio=1)

mem = ramulator.memory_system.GenericDRAM(
    clock_ratio=1,
    controllers=[make_controller(), make_controller()],
    channel_mapper=ramulator.channel_mapper.CacheLineInterleave(),
)

sim = ramulator.Simulation(frontend, mem)
