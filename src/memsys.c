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

  return m;
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
