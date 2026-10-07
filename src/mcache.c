#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "mcache.h"


#define MCACHE_SRRIP_MAX  7
#define MCACHE_SRRIP_INIT 1

static uns  mcache_bard_e(MCache *c, uns set, uns victim);
static int  mcache_bard_c(MCache *c, uns set);
static void mcache_bard_track_writeback(MCache *c, uns bank);
static void mcache_bard_log_event(MCache *c, uns set, uns64 tracker_before, uns victim, char action, int cleanse);

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

MCache *
mcache_new(uns sets, uns assocs, uns repl_policy ){
  MCache *c = (MCache *) calloc (1, sizeof (MCache));
  c->sets    = sets;
  c->assocs  = assocs;
  c->repl_policy = (MCache_ReplPolicy)repl_policy;

  c->entries  = (MCache_Entry *) calloc (sets * assocs, sizeof(MCache_Entry));

  return c;
}




////////////////////////////////////////////////////////////////////
// the addr field is the lineaddress = address/cache_line_size
////////////////////////////////////////////////////////////////////

Flag mcache_access (MCache *c, Addr addr){
  Addr  tag  = addr; // full tags
  uns   set  = mcache_get_index(c,addr);
  uns   start = set * c->assocs;
  uns   end   = start + c->assocs;
  uns   ii;
    
  c->s_count++;
    
  for (ii=start; ii<end; ii++){
    MCache_Entry *entry = &c->entries[ii];
    
    if(entry->valid && (entry->tag == tag))
      {
	entry->last_access  = c->s_count;
	entry->ripctr       = MCACHE_SRRIP_MAX;
	c->touched_wayid = (ii-start);
	c->touched_setid = set;
	c->touched_lineid = ii;
	return HIT;
      }
  }

  //even on a miss, we need to know which set was accessed
  c->touched_wayid = 0;
  c->touched_setid = set;
  c->touched_lineid = start;

  c->s_miss++;
  return MISS;
}


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

Flag    mcache_probe    (MCache *c, Addr addr){
  Addr  tag  = addr; // full tags
  uns   set  = mcache_get_index(c,addr);
  uns   start = set * c->assocs;
  uns   end   = start + c->assocs;
  uns   ii;

  for (ii=start; ii<end; ii++){
    MCache_Entry *entry = &c->entries[ii];
    if(entry->valid && (entry->tag == tag))
      {
	return TRUE;
      }
  }
  
  return FALSE;
}


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

Flag    mcache_invalidate    (MCache *c, Addr addr){
  Addr  tag  = addr; // full tags
  uns   set  = mcache_get_index(c,addr);
  uns   start = set * c->assocs;
  uns   end   = start + c->assocs;
  uns   ii;

  for (ii=start; ii<end; ii++){
    MCache_Entry *entry = &c->entries[ii];
    if(entry->valid && (entry->tag == tag))
      {
	entry->valid = FALSE;
	return TRUE;
      }
  }
  
  return FALSE;
}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

Flag    mcache_mark_dirty    (MCache *c, Addr addr){
  Addr  tag  = addr; // full tags
  uns   set  = mcache_get_index(c,addr);
  uns   start = set * c->assocs;
  uns   end   = start + c->assocs;
  uns   ii;

  for (ii=start; ii<end; ii++){
    MCache_Entry *entry = &c->entries[ii];
    if(entry->valid && (entry->tag == tag))
      {
	if(entry->cleansed){
	  c->s_cleanse_rewritten++;
	  entry->cleansed = FALSE;
	}
	entry->dirty = TRUE;
	return TRUE;
      }
  }
  
  return FALSE;
}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////


void mcache_install (MCache *c, Addr addr){
  Addr  tag  = addr; // full tags
  uns   set  = mcache_get_index(c,addr);
  uns   start = set * c->assocs;
  uns   end   = start + c->assocs;
  uns   ii, victim;
  
  Flag update_lrubits=TRUE;

  c->evicted_dirty_line = FALSE;
  c->cleansed_line = FALSE;

  MCache_Entry *entry;

  for (ii=start; ii<end; ii++){
    entry = &c->entries[ii];
    if(entry->valid && (entry->tag == tag)){
      printf("Installed entry already with addr:%llx present in set:%u\n", addr, set);
      exit(-1);
    }
  }
  
  // find victim and install entry
  victim = mcache_find_victim(c, set);
  entry = &c->entries[victim];

  if(entry->valid){
    uns   lru_victim = victim;
    uns64 tracker_before = c->bard_tracker;
    int   cleanse = -1;   // line to write back and keep (BARD-C)

    if(entry->dirty && (c->bard == BARD_E || c->bard == BARD_H)){
      victim = mcache_bard_e(c, set, victim);
      entry = &c->entries[victim];
    }else if(!entry->dirty && (c->bard == BARD_C || c->bard == BARD_H)){
      cleanse = mcache_bard_c(c, set);
    }

    c->s_evict++;
    char action;
    if(victim != lru_victim){
      c->s_bard_override++;
      action = 'E';
    }else if(cleanse >= 0){
      c->s_bard_cleanse++;
      action = 'C';
    }else{
      c->s_lru_evict++;
      action = 'L';
    }
    if(c->bard_log && c->bard_log_left){
      mcache_bard_log_event(c, set, tracker_before, victim, action, cleanse);
    }
    if(entry->dirty){
      c->s_writeback++;
      c->evicted_dirty_line = TRUE;
      c->evicted_line_addr = entry->tag;
      if(c->bank_of){
	mcache_bard_track_writeback(c, entry->bank);
      }
    }
    if(cleanse >= 0){
      MCache_Entry *ce = &c->entries[cleanse];
      ce->dirty = FALSE;
      ce->cleansed = TRUE;
      c->cleansed_line = TRUE;
      c->cleansed_line_addr = ce->tag;
      mcache_bard_track_writeback(c, ce->bank);
    }
  }

 //udpate DRRIP info and select value of ripctr
  uns ripctr_val=MCACHE_SRRIP_INIT;

  
  //put new information in
  entry->tag   = tag;
  entry->valid = TRUE;
  entry->dirty = FALSE;
  entry->cleansed = FALSE;
  entry->ripctr  = ripctr_val;
  if(c->bank_of){
    entry->bank = c->bank_of(tag);
  }

  if(update_lrubits){
    entry->last_access  = c->s_count;   
  }

  c->touched_lineid=victim;
  c->touched_setid=set;
  c->touched_wayid=victim-(set*c->assocs);
}





////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns mcache_find_victim (MCache *c, uns set)
{
  int ii;
  int start = set   * c->assocs;    
  int end   = start + c->assocs;    

  //search for invalid first
  for (ii = start; ii < end; ii++){
    if(!c->entries[ii].valid){
      return ii;
    }
  }

  switch(c->repl_policy){
  case REPL_LRU: 
    return mcache_find_victim_lru(c, set);
  case REPL_RND: 
    return mcache_find_victim_rnd(c, set);
  case REPL_SRRIP: 
    return mcache_find_victim_srrip(c, set);
  default: 
    assert(0);
  }

  return -1;

}


////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns
mcache_find_victim_lru (MCache *c,  uns set)
{
  uns start = set   * c->assocs;    
  uns end   = start + c->assocs;    
  uns lowest=start;
  uns ii;


  for (ii = start; ii < end; ii++){
    if (c->entries[ii].last_access < c->entries[lowest].last_access){
      lowest = ii;
    }
  }

  return lowest;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns
mcache_find_victim_rnd (MCache *c,  uns set)
{
    uns start = set   * c->assocs;    
    uns victim = start + rand()%c->assocs; 

    return  victim;   
}



////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns
mcache_find_victim_srrip (MCache *c,  uns set)
{
  uns start = set   * c->assocs;    
  uns end   = start + c->assocs;    
  uns ii;
  uns victim = end; // init to impossible

  while(victim == end){
    for (ii = start; ii < end; ii++){
      if (c->entries[ii].ripctr == 0){
	victim = ii;
	break;
      }
    }
    
    if(victim == end){
      for (ii = start; ii < end; ii++){
	c->entries[ii].ripctr--;
      }
    }
  }

  return  victim;   
}




////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

uns mcache_get_index(MCache *c, Addr addr){
  uns retval;

  switch(c->index_policy){
  case 0:
    retval=addr%c->sets;
    break;

  default:
    exit(-1);
  }

  return retval;
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void  mcache_reset_stats(MCache *c){
  c->s_count_base = c->s_count; // s_count is the LRU clock, so keep it running
  c->s_miss = 0;
  c->s_evict = 0;
  c->s_writeback = 0;
  c->s_lru_evict = 0;
  c->s_bard_override = 0;
  c->s_bard_cleanse = 0;
  c->s_cleanse_rewritten = 0;
  c->s_tracker_reset = 0;   // the BLP-Tracker's bits are cache state: kept
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////

void  mcache_print_stats(MCache *c, char *header){
  uns64 accesses = c->s_count - c->s_count_base;
  double missrate = 100.0 * (double)c->s_miss/(double)accesses;

  printf("\n%s_ACCESS       \t : %llu",  header,  accesses);
  printf("\n%s_MISS         \t : %llu",  header,  c->s_miss);
  printf("\n%s_MISSRATE     \t : %6.3f", header,  missrate);
  printf("\n%s_WRITEBACKS   \t : %llu",  header,  c->s_writeback);
  printf("\n%s_BARD         \t : %u",    header,  (uns)c->bard);
  printf("\n%s_EVICTIONS    \t : %llu",  header,  c->s_evict);
  printf("\n%s_LRU_EVICTS   \t : %llu",  header,  c->s_lru_evict);
  printf("\n%s_BARD_OVERRIDES\t : %llu", header,  c->s_bard_override);
  printf("\n%s_BARD_CLEANSES\t : %llu",  header,  c->s_bard_cleanse);
  printf("\n%s_CLEANSED_REWRITTEN\t : %llu", header, c->s_cleanse_rewritten);
  uns64 dirty = 0;  // dirty lines in the cache right now (not a counter)
  for(uns64 ii = 0; ii < (uns64)c->sets * c->assocs; ii++){
    dirty += c->entries[ii].valid && c->entries[ii].dirty;
  }
  printf("\n%s_DIRTY_LINES  \t : %llu",  header,  dirty);
  printf("\n%s_TRACKER_RESETS\t : %llu", header,  c->s_tracker_reset);
  printf("\n");
}

////////////////////////////////////////////////////////////
// BARD: Bank-Aware Replacement Decisions (Vittal & Qureshi, HPCA 2026).
// Works on top of LRU. bank_of gives a line's DRAM bank, numbered
// sub-channel by sub-channel (sub-channel = bank / banks_per_subch).
////////////////////////////////////////////////////////////

void mcache_bard_init(MCache *c, uns bard, uns (*bank_of)(Addr lineaddr), uns num_banks, uns banks_per_subch){
  assert(c->repl_policy == REPL_LRU || bard == BARD_OFF);
  assert(c->assocs <= MCACHE_MAX_ASSOC);
  assert(num_banks >= 1 && num_banks <= 64);
  assert(banks_per_subch >= 1 && num_banks % banks_per_subch == 0);
  c->bard = (MCache_Bard)bard;
  c->bank_of = bank_of;
  c->bard_num_banks = num_banks;
  c->bard_banks_per_subch = banks_per_subch;
  c->bard_tracker = 0;
}

static Flag mcache_bard_pending(MCache *c, uns bank){
  return (c->bard_tracker >> bank) & 1;
}

////////////////////////////////////////////////////////////
// BLP-Tracker update on every writeback the LLC sends to memory: set the
// bank's bit; if that makes all bits of its sub-channel 1, clear them.
////////////////////////////////////////////////////////////

static void mcache_bard_track_writeback(MCache *c, uns bank){
  uns   n    = c->bard_banks_per_subch;
  uns64 mask = ((n == 64) ? ~0ULL : ((1ULL << n) - 1)) << (bank / n * n);
  c->bard_tracker |= 1ULL << bank;
  if((c->bard_tracker & mask) == mask){
    c->bard_tracker &= ~mask;
    c->s_tracker_reset++;
  }
}

////////////////////////////////////////////////////////////
// Line indices of a set, least recently used first (equal last_access:
// lower way first, as mcache_find_victim_lru picks).
////////////////////////////////////////////////////////////

static void mcache_lru_order(MCache *c, uns set, uns *order){
  uns start = set * c->assocs;
  for(uns ii = 0; ii < c->assocs; ii++){
    uns jj = ii;
    while(jj > 0 && c->entries[order[jj-1]].last_access > c->entries[start+ii].last_access){
      order[jj] = order[jj-1];
      jj--;
    }
    order[jj] = start + ii;
  }
}

////////////////////////////////////////////////////////////
// BARD-E: only when the LRU victim is dirty and its bank has a pending
// write. Then scan the other lines from LRU to MRU and evict the first
// dirty line whose bank has no pending write; if none, evict the LRU line.
////////////////////////////////////////////////////////////

static uns mcache_bard_e(MCache *c, uns set, uns victim){
  MCache_Entry *lru = &c->entries[victim];
  if(!lru->dirty || !mcache_bard_pending(c, lru->bank)){
    return victim;
  }

  uns order[MCACHE_MAX_ASSOC];
  mcache_lru_order(c, set, order);
  for(uns ii = 0; ii < c->assocs; ii++){
    MCache_Entry *e = &c->entries[order[ii]];
    if(order[ii] != victim && e->valid && e->dirty && !mcache_bard_pending(c, e->bank)){
      return order[ii];
    }
  }
  return victim;
}

////////////////////////////////////////////////////////////
// BARD-C: only when the LRU victim is clean (it is evicted as usual). Scan
// the set from LRU to MRU for a dirty line whose bank has no pending
// write; that line is written back and stays cached, clean. Returns its
// line index, or -1 if there is none.
////////////////////////////////////////////////////////////

static int mcache_bard_c(MCache *c, uns set){
  uns order[MCACHE_MAX_ASSOC];
  mcache_lru_order(c, set, order);
  for(uns ii = 0; ii < c->assocs; ii++){
    MCache_Entry *e = &c->entries[order[ii]];
    if(e->valid && e->dirty && !mcache_bard_pending(c, e->bank)){
      return order[ii];
    }
  }
  return -1;
}

////////////////////////////////////////////////////////////
// -bardlog: one line per eviction (the first max_events), with the set as
// it was before the decision, so SCRIPTS/check_bard.py can redo every
// decision and the BLP-Tracker independently:
//   <set> <tracker before, hex> <evicted way> <L|E|C> <cleansed way, -1 if none>
//   then per way <last_access>,<dirty>,<line address>,<bank>
////////////////////////////////////////////////////////////

void mcache_bard_log(MCache *c, const char *fname, uns64 max_events){
  c->bard_log = fopen(fname, "w");
  if(c->bard_log == NULL){
    fprintf(stderr, "cannot write the BARD log %s\n", fname);
    exit(-1);
  }
  c->bard_log_left = max_events;
  fprintf(c->bard_log, "# bard %u assoc %u banks %u banks_per_subch %u sets %u\n",
	  (uns)c->bard, c->assocs, c->bard_num_banks, c->bard_banks_per_subch, c->sets);
}

static void mcache_bard_log_event(MCache *c, uns set, uns64 tracker_before, uns victim, char action, int cleanse){
  uns start = set * c->assocs;
  fprintf(c->bard_log, "%u %016llx %u %c %d", set, tracker_before, victim - start, action,
	  cleanse >= 0 ? (int)(cleanse - start) : -1);
  for(uns ii = start; ii < start + c->assocs; ii++){
    MCache_Entry *e = &c->entries[ii];
    fprintf(c->bard_log, " %llu,%u,%llu,%u", e->last_access, (uns)e->dirty, e->tag, e->bank);
  }
  fprintf(c->bard_log, "\n");
  if(--c->bard_log_left == 0){
    fclose(c->bard_log);
    c->bard_log = NULL;
  }
}





