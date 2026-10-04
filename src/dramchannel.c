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
    if(c->bank[ii]->sleep_cycle > max_bank_ready_cycle){
      max_bank_ready_cycle = c->bank[ii]->sleep_cycle;
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
    if(c->bank[ii]->sleep_cycle > max_bank_ready_cycle){
      max_bank_ready_cycle = c->bank[ii]->sleep_cycle;
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
  return dram_bank_insert(c->bank[bankid],type, rowid, lineaddr);
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
	if(dram_channel_get_rdwr_token(c, cycle, index, &ccd_delay)){
	  uns64 rdwr_bus_delay = tCAS+ccd_delay;

	  if(c->dbusq.entries[index].reqtype == DRAM_REQ_RD){
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
	  c->bank[index]->sleep_cycle = (cycle+tCCDL); // bank busy -- same bank RD-to-RD is tCCDL
	}
	return; // if checked token, then no more token left
      }
    }
  }

}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////


void   dram_channel_return_reads(DRAM_Channel *c){
  while(c->rdretq.size > 0 && cycle >= c->rdretq.done_time[c->rdretq.head]){
    memsys_callback(memsys, c->rdretq.lineaddr[c->rdretq.head]);
    c->rdretq.head = (c->rdretq.head + 1) % NUM_DRAM_RDRETQ_ENTRIES;
    c->rdretq.size--;
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

Flag   dram_channel_get_rdwr_token(DRAM_Channel *c, uns64 in_cycle, uns64 my_bank, uns64 *out_delay){
  // Matches Ramulator2's DDR5 RD-to-RD constraints (single rank):
  //   any bank on the channel          -> tCCDS (checked here)
  //   other bank, same bank group      -> tCCDM (checked here)
  //   same bank                        -> tCCDL (enforced by bank sleep_cycle after dispatch)
  DRAM_RDWR_Token *t = &c->rdwr_token;
  uns64 my_bankgroup = my_bank / c->banks_in_bankgroup;

  if(t->has_prev && (in_cycle - t->prev_time) < tCCDS){
    return FALSE;
  }
  if(t->bg_has_prev[my_bankgroup] && (in_cycle - t->bg_prev_time[my_bankgroup]) < tCCDM){
    return FALSE;
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
