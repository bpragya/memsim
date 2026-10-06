#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "externs.h"
#include "memsys.h"
#include "mcore.h"


extern MCore *mcore[MAX_THREADS];

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

MemSys *memsys_new(void){
  MemSys *m = new MemSys;
  
  // init main memory DRAM
  m->mainmem = dram_new(); 
  sprintf(m->mainmem->name, "MEM");
  m->lines_in_mainmem_rbuf = MEM_PAGESIZE/LINESIZE; // static
  m->s_wb_delayed = 0;
  m->s_wb_stalls  = 0;
  m->s_wbbuf_max  = 0;

  return m;
}


//////////////////////////////////////////////////////////////////////////
// NOTE: ACCESSES TO THE MEMORY USE LINEADDR OF THE CACHELINE ACCESSED
//////////////////////////////////////////////////////////////////////////

Flag memsys_access(MemSys *m, Addr lineaddr,  uns coreid, uns robid, uns64 inst_num, Addr wb_lineaddr){
  Flag retval=FALSE;

  // back-pressure: too many writebacks waiting for the write queue, so
  // refuse the read (the core sleeps and retries it, with no writeback)
  if(m->wbbuf.size() >= MEM_WBBUF_SIZE){
    m->s_wb_stalls++;
  }else{
    retval = dram_insert(m->mainmem, lineaddr, FALSE);
    if(retval==TRUE){
      memsys_mshr_insert(m, lineaddr, coreid, robid, inst_num);
    }
  }

  // a writeback is never dropped: if the write queue refuses it, it waits
  // in wbbuf and is retried every cycle in memsys_cycle()
  if(wb_lineaddr){
    if(!dram_insert(m->mainmem, wb_lineaddr, TRUE)){
      m->wbbuf.push_back(wb_lineaddr);
      m->s_wb_delayed++;
      if(m->wbbuf.size() > m->s_wbbuf_max){
	m->s_wbbuf_max = m->wbbuf.size();
      }
    }
  }
  return retval;
}


//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  memsys_cycle(MemSys *m){
  dram_cycle(m->mainmem);

  // retry waiting writebacks, oldest first; one blocked channel does not
  // hold back writebacks to other channels
  for(auto it = m->wbbuf.begin(); it != m->wbbuf.end(); ){
    if(dram_insert(m->mainmem, *it, TRUE)){
      it = m->wbbuf.erase(it);
    }else{
      ++it;
    }
  }
}

//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  memsys_print_state(MemSys *m){
  dram_print_state(m->mainmem);
}

//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  memsys_print_stats(MemSys *m){
  dram_print_stats(m->mainmem);
  printf("MEM_WB_DELAYED  \t : %llu\n", m->s_wb_delayed);
  printf("MEM_WB_STALLS   \t : %llu\n", m->s_wb_stalls);
  printf("MEM_WBBUF_MAX   \t : %llu\n", m->s_wbbuf_max);
}

//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
void    memsys_mshr_insert(MemSys *m, Addr lineaddr, uns coreid, uns robid, uns64 inst_num){
  MSHR_Entry entry;
  entry.coreid = coreid;
  entry.robid = robid;
  entry.inst_num = inst_num;
  assert(m->mshr.count(lineaddr)==0); // the line should not be present already
  m->mshr[lineaddr] = entry;
}

//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
void    memsys_callback(MemSys *m, Addr lineaddr){
  assert(m->mshr.count(lineaddr)>0);
  MSHR_Entry entry = m->mshr[lineaddr];
  mcore_rob_wakeup(mcore[entry.coreid], entry.robid, entry.inst_num);
  m->mshr.erase(lineaddr);
}

//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
