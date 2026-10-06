#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "mcore.h"
#include "externs.h"
#include "dramchannel.h"
#include  "memsys.h"

extern MemSys *memsys;


extern uns64 cycle;

uns64 tFAW=12*4;

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

DRAM_Channel*   dram_channel_new(uns id, uns num_banks, uns num_rows){
  DRAM_Channel *c = (DRAM_Channel *) calloc (1, sizeof (DRAM_Channel));
  c->id = id;
  c->num_banks = num_banks;
  c->num_rows = num_rows;
  c->banks_in_bankgroup = num_banks/DRAM_BANKGROUPS;

  for(uns ii=0; ii< num_banks; ii++){
    c->bank[ii] = dram_bank_new(ii, id, num_rows/num_banks);
  }

  c->tfaw_token.tfaw_time = tFAW;
  c->rdwr_token.has_prev = FALSE;
  return c;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void dram_channel_rfmab(DRAM_Channel *c){
  uns ii;
  uns64 max_bank_ready_cycle = cycle;

  for(ii=0; ii < c->num_banks; ii++){
    if(dram_bank_ready_cycle(c->bank[ii]) > max_bank_ready_cycle){
      max_bank_ready_cycle = dram_bank_ready_cycle(c->bank[ii]);
    }
  }

  for(ii=0; ii < c->num_banks; ii++){
    dram_bank_rfmab(c->bank[ii],max_bank_ready_cycle); // no bank-q
  }

}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void dram_channel_rfmsb(DRAM_Channel *c){
  uns ii;
  uns64 max_bank_ready_cycle = cycle;
  uns val = c->RFMSB;
  uns pos=0;

  //-- find the position of first one from left to right
  for(uns ii=0; ii<32; ii++){
    if(val%2==1){
      pos=ii;
      break;
    }
    val/=2;
  }

  c->RFMSB -=  (1<< pos); // reset that bit in RFMSB

  uns start= pos *  c->banks_in_bankgroup; 
  uns end = start +  c->banks_in_bankgroup; 
  for(ii=start; ii < end; ii++){
    if(dram_bank_ready_cycle(c->bank[ii]) > max_bank_ready_cycle){
      max_bank_ready_cycle = dram_bank_ready_cycle(c->bank[ii]);
    }
  }

  for(ii=start; ii < end; ii++){
    dram_bank_rfmsb(c->bank[ii],max_bank_ready_cycle); // no bank-q
  }

}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void  dram_channel_cycle(DRAM_Channel *c){
  uns ii;

  dram_channel_return_reads(c);
  dram_channel_schedule_rdwrq(c);

  dram_channel_purge_wrq(c);
  dram_channel_set_write_mode(c);
  if(c->write_mode && c->wrq.size){
    c->s_wr_mode_cycles++;
  }

  for(ii=0; ii<c->num_banks; ii++){
    dram_bank_cycle(c->bank[ii]);
  }



  if(c->RFMAB){
    c->RFMAB=FALSE;
    c->s_RFM++;
    dram_channel_rfmab(c);
  }

   if(c->RFMSB){
    c->s_RFM++;
    dram_channel_rfmsb(c);
  }

  
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns   dram_channel_insert(DRAM_Channel *c, uns bankid,  DRAM_ReqType type, uns64 rowid, Addr lineaddr){

  assert(bankid < c->num_banks);
  uns retval = dram_bank_insert(c->bank[bankid],type, rowid, lineaddr);
  if(retval && type == DRAM_REQ_RD){
    c->reads_waiting++;
  }
  return retval;
}


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  dram_channel_insert_rdwrq(DRAM_Channel *c, uns bankid, DRAM_ReqType reqtype, Addr lineaddr, uns64 ready_time){

  assert(c->dbusq.entries[bankid].valid == FALSE);

  c->dbusq.entries[bankid].valid = TRUE;
  c->dbusq.entries[bankid].lineaddr = lineaddr;
  c->dbusq.entries[bankid].reqtype = reqtype;
  c->dbusq.entries[bankid].ready_time = ready_time;
  c->dbusq.size++;

}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void   dram_channel_schedule_rdwrq(DRAM_Channel *c){

  uns offset = cycle%c->num_banks; // avoid starvation, start roundrobin

   for(uns ii=0; ii<c->num_banks; ii++){
     uns index = (ii+offset)%c->num_banks;
     
    if(c->dbusq.entries[index].valid == TRUE){
      if(cycle >= c->dbusq.entries[index].ready_time){
	uns64 ccd_delay;
	DRAM_ReqType reqtype = c->dbusq.entries[index].reqtype;
	if(dram_channel_get_rdwr_token(c, cycle, index, reqtype, &ccd_delay)){
	  uns64 rdwr_bus_delay = (reqtype == DRAM_REQ_WB ? tCWL : tCAS) + ccd_delay;

	  if(reqtype == DRAM_REQ_RD){
	    // data returns to the core tCAS+tBUS after the RD is issued
	    assert(c->rdretq.size < NUM_DRAM_RDRETQ_ENTRIES);
	    uns tail = (c->rdretq.head + c->rdretq.size) % NUM_DRAM_RDRETQ_ENTRIES;
	    c->rdretq.lineaddr[tail]  = c->dbusq.entries[index].lineaddr;
	    c->rdretq.done_time[tail] = cycle + tCAS + tBUS;
	    c->rdretq.size++;
	  }
	  c->s_bus_time += rdwr_bus_delay;
	  c->dbusq.size--;
	  c->dbusq.entries[index].valid = FALSE;
	  if(reqtype == DRAM_REQ_WB){
	    c->bank[index]->sleep_cycle = (cycle+tCCDL_WR); // same bank WR-to-WR is tCCDL_WR
	    c->bank[index]->last_wr_cycle = cycle;           // PRE must wait for write recovery
	    c->bank[index]->has_wr = TRUE;
	    c->s_wr_issued++;
	    if(c->dbusq.entries[index].leave_at_dispatch){
	      dram_channel_retire_wr(c, c->dbusq.entries[index].wrq_index);
	    }
	  }else{
	    c->bank[index]->sleep_cycle = (cycle+tCCDL); // bank busy -- same bank RD-to-RD is tCCDL
	  }
	  return; // one column command per cycle
	}
	// blocked by timing: try the next ready request (FR-FCFS)
      }
    }
  }

}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////


void   dram_channel_return_reads(DRAM_Channel *c){
  DRAM_RdRetQ *qs[2] = {&c->fwdq, &c->rdretq};
  for(uns qq=0; qq<2; qq++){
    DRAM_RdRetQ *q = qs[qq];
    while(q->size > 0 && cycle >= q->done_time[q->head]){
      memsys_callback(memsys, q->lineaddr[q->head]);
      q->head = (q->head + 1) % NUM_DRAM_RDRETQ_ENTRIES;
      q->size--;
    }
  }
}

////////////////////////////////////////////////////////////////////
// Write queue -- mirrors Ramulator2's ControllerBase::send() for writes:
// a write to a line already queued is coalesced, otherwise it needs a
// free slot (else refused).
////////////////////////////////////////////////////////////////////

Flag   dram_channel_insert_wr(DRAM_Channel *c, uns bankid, uns64 rowid, Addr lineaddr){
  assert(bankid < c->num_banks);
  assert(MEM_WRQ_SIZE <= NUM_DRAM_WRQ_MAX);
  int free_slot = -1;

  for(uns ii=0; ii<MEM_WRQ_SIZE; ii++){
    if(c->wrq.entries[ii].valid){
      if(c->wrq.entries[ii].lineaddr == lineaddr){
	c->s_wr_coalesced++;
	return TRUE;
      }
    }else if(free_slot < 0){
      free_slot = ii;
    }
  }

  if(free_slot < 0){
    c->s_wrq_full++;
    return FALSE;
  }

  DRAM_WrQ_Entry *e = &c->wrq.entries[free_slot];
  e->valid    = TRUE;
  e->in_service = FALSE;
  e->lineaddr = lineaddr;
  e->bankid   = bankid;
  e->rowid    = rowid;
  e->arrive   = cycle;
  c->wrq.size++;
  c->wrq.per_bank[bankid]++;
  return TRUE;
}

////////////////////////////////////////////////////////////////////
// A read to a line with a queued write is served from the write queue
// one cycle later, without going to DRAM (Ramulator2 read forwarding).
////////////////////////////////////////////////////////////////////

Flag   dram_channel_forward_rd(DRAM_Channel *c, Addr lineaddr){
  if(c->wrq.size == 0){
    return FALSE;
  }
  for(uns ii=0; ii<MEM_WRQ_SIZE; ii++){
    if(c->wrq.entries[ii].valid && c->wrq.entries[ii].lineaddr == lineaddr){
      assert(c->fwdq.size < NUM_DRAM_RDRETQ_ENTRIES);
      uns tail = (c->fwdq.head + c->fwdq.size) % NUM_DRAM_RDRETQ_ENTRIES;
      c->fwdq.lineaddr[tail]  = lineaddr;
      c->fwdq.done_time[tail] = cycle + 1;
      c->fwdq.size++;
      c->s_rd_forwarded++;
      return TRUE;
    }
  }
  return FALSE;
}

////////////////////////////////////////////////////////////////////
// Pick the next write for bank b: row hit first, then oldest (FR-FCFS).
// Returns the write queue index, or -1 if the bank has no queued write.
////////////////////////////////////////////////////////////////////

int    dram_channel_pick_wr(DRAM_Channel *c, DRAM_Bank *b){
  if(c->wrq.per_bank[b->id] == 0){
    return -1;
  }

  int oldest = -1, oldest_hit = -1;
  for(uns ii=0; ii<MEM_WRQ_SIZE; ii++){
    DRAM_WrQ_Entry *e = &c->wrq.entries[ii];
    if(!e->valid || e->in_service || e->bankid != b->id){
      continue;
    }
    if(oldest < 0 || e->arrive < c->wrq.entries[oldest].arrive){
      oldest = ii;
    }
    if(b->row_valid && e->rowid == b->open_row_id){
      if(oldest_hit < 0 || e->arrive < c->wrq.entries[oldest_hit].arrive){
	oldest_hit = ii;
      }
    }
  }

  if(DRAM_SCHED_POLICY == DRAM_SCHED_FRFCFS && oldest_hit >= 0){
    return oldest_hit;
  }
  return oldest;
}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

// bank picked this write: it stops being schedulable, but (like Ramulator2)
// stays in the write queue until its ACT issues, or its WR if a row hit
void   dram_channel_start_wr(DRAM_Channel *c, uns index, Flag opened, uns64 act_time){
  DRAM_WrQ_Entry *e = &c->wrq.entries[index];
  assert(e->valid && !e->in_service);
  c->wrq.per_bank[e->bankid]--;
  e->in_service = TRUE;
  e->leave_at_dispatch = !opened;
  e->leave_time = act_time;
  c->wrq.n_inservice++;
  if(opened && act_time <= cycle){
    dram_channel_retire_wr(c, index); // ACT issues right away
  }
}

void   dram_channel_retire_wr(DRAM_Channel *c, uns index){
  DRAM_WrQ_Entry *e = &c->wrq.entries[index];
  assert(e->valid && e->in_service);
  e->valid = FALSE;
  e->in_service = FALSE;
  c->wrq.n_inservice--;
  c->wrq.size--;
}

// writes whose ACT has now issued leave the write queue
void   dram_channel_purge_wrq(DRAM_Channel *c){
  if(c->wrq.n_inservice == 0){
    return;
  }
  for(uns ii=0; ii<MEM_WRQ_SIZE; ii++){
    DRAM_WrQ_Entry *e = &c->wrq.entries[ii];
    if(e->valid && e->in_service && !e->leave_at_dispatch && cycle >= e->leave_time){
      dram_channel_retire_wr(c, ii);
    }
  }
}

// Ramulator2's read buffer size: reads not yet picked by their bank, plus
// picked reads whose ACT (or RD, for a row hit) has not issued yet
uns64  dram_channel_reads_in_buffer(DRAM_Channel *c){
  uns64 count = c->reads_waiting;
  for(uns ii=0; ii<c->num_banks; ii++){
    DRAM_BusQ_Entry *e = &c->dbusq.entries[ii];
    if(e->valid && e->reqtype == DRAM_REQ_RD){
      if(e->leave_at_dispatch || cycle < e->leave_time){
	count++;
      }
    }
  }
  return count;
}

////////////////////////////////////////////////////////////////////
// Same rule as Ramulator2's ControllerBase::set_write_mode():
//   enter write mode when WRQ > high watermark, or no reads are waiting
//   leave write mode when WRQ < low watermark and reads are waiting
////////////////////////////////////////////////////////////////////

void   dram_channel_set_write_mode(DRAM_Channel *c){
  uns64 reads = dram_channel_reads_in_buffer(c);
  if(!c->write_mode){
    Flag above_high = (c->wrq.size > MEM_WRQ_HIGH_WM * MEM_WRQ_SIZE);
    if(above_high || reads == 0){
      c->write_mode = TRUE;
      if(above_high){
	c->s_wr_drains++; // a forced drain: reads wait while it runs
      }
    }
  }else{
    if( (c->wrq.size < MEM_WRQ_LOW_WM * MEM_WRQ_SIZE) && reads != 0){
      c->write_mode = FALSE;
    }
  }
}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////


Flag   dram_channel_get_tfaw_token(DRAM_Channel *c, uns64 in_cycle){
  if( (in_cycle -  c->tfaw_token.prev_time[0]) >= c->tfaw_token.tfaw_time){
    c->tfaw_token.prev_time[0] = c->tfaw_token.prev_time[1];
    c->tfaw_token.prev_time[1] = c->tfaw_token.prev_time[2];
    c->tfaw_token.prev_time[2] = c->tfaw_token.prev_time[3];
    c->tfaw_token.prev_time[3] = in_cycle;
    return TRUE;
  }

  return FALSE;
}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

Flag   dram_channel_get_rdwr_token(DRAM_Channel *c, uns64 in_cycle, uns64 my_bank, DRAM_ReqType type, uns64 *out_delay){
  // Matches Ramulator2's DDR5 column-command constraints (single rank).
  //   RD after RD : any bank tCCDS, same bank group tCCDM (checked here),
  //                 same bank tCCDL (bank sleep_cycle after dispatch)
  //   RD after WR : tCWL+tBUS + tWTRS (any) / tWTRM (same bank group) / tWTRL (same bank)
  //   WR after RD : tRTW (any bank)
  //   WR after WR : tCCDS_WR (any) / tCCDM_WR (same bank group) / tCCDL_WR (same bank)
  DRAM_RDWR_Token *t = &c->rdwr_token;
  uns64 my_bankgroup = my_bank / c->banks_in_bankgroup;
  uns64 bg_start = my_bankgroup * c->banks_in_bankgroup;
  uns64 bg_end   = bg_start + c->banks_in_bankgroup;

  if(type == DRAM_REQ_WB){
    if(t->has_prev_wr && (in_cycle - t->prev_wr_time) < tCCDS_WR){
      return FALSE;
    }
    if(t->has_prev && (in_cycle - t->prev_time) < tRTW){
      return FALSE;
    }
    for(uns64 bb=bg_start; bb<bg_end; bb++){
      uns64 need = (bb == my_bank) ? tCCDL_WR : tCCDM_WR;
      if(t->bank_has_wr[bb] && (in_cycle - t->bank_wr_time[bb]) < need){
	return FALSE;
      }
    }

    t->prev_wr_time = in_cycle;
    t->has_prev_wr = TRUE;
    t->bank_wr_time[my_bank] = in_cycle;
    t->bank_has_wr[my_bank] = TRUE;
    *out_delay = tCCDS_WR;
    return TRUE;
  }

  if(t->has_prev && (in_cycle - t->prev_time) < tCCDS){
    return FALSE;
  }
  if(t->bg_has_prev[my_bankgroup] && (in_cycle - t->bg_prev_time[my_bankgroup]) < tCCDM){
    return FALSE;
  }
  if(t->has_prev_wr && (in_cycle - t->prev_wr_time) < tCWL + tBUS + tWTRS){
    return FALSE;
  }
  for(uns64 bb=bg_start; bb<bg_end; bb++){
    uns64 need = tCWL + tBUS + ((bb == my_bank) ? tWTRL : tWTRM);
    if(t->bank_has_wr[bb] && (in_cycle - t->bank_wr_time[bb]) < need){
      return FALSE;
    }
  }

  // spacing that applied relative to the previous dispatch (for bus-delay stats)
  uns64 delay = tCCDS;
  if(t->has_prev && (my_bank == t->prev_bank)){
    delay = tCCDL;
  }else if(t->has_prev && (my_bankgroup == t->prev_bank / c->banks_in_bankgroup)){
    delay = tCCDM;
  }

  t->prev_time = in_cycle;
  t->prev_bank = my_bank;
  t->has_prev = TRUE;
  t->bg_prev_time[my_bankgroup] = in_cycle;
  t->bg_has_prev[my_bankgroup] = TRUE;
  *out_delay = delay;
  return TRUE;
}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void   dram_channel_print_state(DRAM_Channel *c){
  printf("\nCh: %u\t", c->id);
  printf("BusQ: %2u\t", c->dbusq.size);
  for(uns ii=0; ii< c->num_banks; ii++){
    printf("%2u ",(uns)c->bank[ii]->bankq.size());
  }
}

///////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
