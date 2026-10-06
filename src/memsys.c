#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>

#include "externs.h"
#include "memsys.h"
#include "mcore.h"

#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/controller/controller_base.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"

extern MCore *mcore[MAX_THREADS];
extern uns64  LINESIZE;
extern char   RAMULATOR_CONFIG_PATH[1024];

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

struct MemSys {
  Ramulator::IFrontEnd     *frontend;
  Ramulator::IMemorySystem *memory_system;
};

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

MemSys *memsys_new(void){
  MemSys *m = new MemSys;

  Ramulator::ConfigNode config = Ramulator::Config::parse_config_file(RAMULATOR_CONFIG_PATH);

  m->frontend      = Ramulator::Factory::create_frontend(config);
  m->memory_system = Ramulator::Factory::create_memory_system(config);

  m->frontend->connect_memory_system(m->memory_system);
  m->memory_system->connect_frontend(m->frontend);

  memsys_print_dram_timings(m);
  return m;
}

//////////////////////////////////////////////////////////////////////////
// Print the DRAM timings Ramulator2 is actually using, next to the BARD
// paper's Table I (DDR5-4800B x4), so the configuration can be checked.
//////////////////////////////////////////////////////////////////////////

// Ramulator2 keeps an Implementation's children protected and has no public
// accessor for the DRAM spec. A pointer-to-member taken inside a derived
// class is the standard way to read a protected member without changing
// Ramulator2 itself.
struct ImplChildren : Ramulator::Implementation {
  static std::vector<std::unique_ptr<Ramulator::Implementation>>& of(Ramulator::Implementation *impl){
    return impl->*(&ImplChildren::m_children);
  }
};

static Ramulator::DRAMSpec* memsys_dram_spec(MemSys *m){
  auto *impl = dynamic_cast<Ramulator::Implementation*>(m->memory_system);
  if(impl == nullptr){
    return nullptr;
  }
  for(auto &child : ImplChildren::of(impl)){
    auto *ctrl = dynamic_cast<Ramulator::ControllerBase*>(child.get());
    if(ctrl != nullptr){
      return ctrl->m_device.m_spec; // all channels share one spec
    }
  }
  return nullptr;
}

void  memsys_print_dram_timings(MemSys *m){
  struct Row { const char *name; const char *desc; const char *ram2; double paper_ns; int paper_cyc; };
  static const Row rows[] = {
    {"CL",        "Read Latency",                "nCL",      16.6, 40},
    {"CWL",       "Write Latency",               "nCWL",     15.8, 38},
    {"tRCD",      "Activate-to-RW Latency",      "nRCD",     16.6, 39},
    {"tRP",       "Precharge-to-Activate",       "nRP",      16.6, 39},
    {"tRAS",      "Activate-to-Precharge",       "nRAS",     32.1, 77},
    {"tWR",       "Write-to-Precharge",          "nWR",      30.4, 72},
    {"BL/2",      "Time to send 64B on bus",     "nBL",       3.3,  8},
    {"tCCD_S_WR", "Write-to-Write (diff. BG)",   "nCCDS_WR",  3.3,  8},
    {"tCCD_L_WR", "Write-to-Write (same BG)",    "nCCDM_WR", 20.4, 48},
    {"",          "  same bank (Ramulator2 only)","nCCDL_WR", 20.4, 48},
  };

  Ramulator::DRAMSpec *spec = memsys_dram_spec(m);
  if(spec == nullptr){
    printf("DRAM timings: could not read Ramulator2's DRAM spec\n");
    return;
  }
  int    rate  = spec->has_timing("rate") ? spec->get_timing_value("rate") : -1;
  double tck   = spec->get_timing_value("tCK_ps") / 1000.0;

  printf("\nDRAM TIMINGS: Ramulator2 %s-%d (tCK %.3f ns) vs BARD Table I (DDR5-4800B x4, tCK 0.416 ns)\n",
	 spec->standard_name.c_str(), rate, tck);
  printf("  %-10s %-30s %-9s %8s %8s | %8s %8s | %s\n",
	 "Name", "Description", "Ram2", "cycles", "ns", "paper", "paper_ns", "match");
  for(const Row &r : rows){
    if(!spec->has_timing(r.ram2)){
      printf("  %-10s %-30s %-9s %8s\n", r.name, r.desc, r.ram2, "n/a");
      continue;
    }
    int    cyc = spec->get_timing_value(r.ram2);
    double ns  = cyc * tck;
    printf("  %-10s %-30s %-9s %8d %8.1f | %8d %8.1f | %s\n",
	   r.name, r.desc, r.ram2, cyc, ns, r.paper_cyc, r.paper_ns,
	   (cyc == r.paper_cyc) ? "yes" : "NO");
  }
  printf("\n");
}


//////////////////////////////////////////////////////////////////////////
// NOTE: ACCESSES TO THE MEMORY USE LINEADDR OF THE CACHELINE ACCESSED
// (i.e. byte_addr / LINESIZE) -- Ramulator2 wants byte addresses, so we
// scale by LINESIZE at this boundary.
//////////////////////////////////////////////////////////////////////////

Flag memsys_access(MemSys *m, Addr lineaddr, uns coreid, uns robid, uns64 inst_num, Addr wb_lineaddr){

  auto on_read_done = [coreid, robid, inst_num](Ramulator::Request &req){
    mcore_rob_wakeup(mcore[coreid], robid, inst_num);
  };

  bool accepted = m->frontend->receive_external_requests(
      Ramulator::Request::Type::Read,
      (Ramulator::Addr_t)(lineaddr * LINESIZE),
      (int)coreid,
      on_read_done,
      (int)LINESIZE);

  if(wb_lineaddr){
    // Best-effort: mirrors the original dram_insert(..., wb) call, whose
    // accept/reject result was never checked either -- writebacks don't
    // block anything, so a dropped writeback here is not observable by
    // the core.
    auto on_wb_done = [](Ramulator::Request &req){};
    m->frontend->receive_external_requests(
        Ramulator::Request::Type::Write,
        (Ramulator::Addr_t)(wb_lineaddr * LINESIZE),
        (int)coreid,
        on_wb_done,
        (int)LINESIZE);
  }

  return accepted ? TRUE : FALSE;
}


//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  memsys_cycle(MemSys *m){
  m->memory_system->tick();
}

//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  memsys_print_stats(MemSys *m){
  m->memory_system->print_stats(std::cout);
}
