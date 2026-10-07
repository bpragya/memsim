#ifndef MCACHE_H
#define MCACHE_H

#include <stdio.h>
#include "global_types.h"


typedef enum MCache_ReplPolicy_Enum {
    REPL_LRU=0,
    REPL_RND=1,
    REPL_SRRIP=2, 
    NUM_REPL_POLICY=3
} MCache_ReplPolicy;


typedef struct MCache_Entry MCache_Entry;
typedef struct MCache MCache;

///////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

struct MCache_Entry {
    Flag    valid;
    Flag    dirty;
    Addr    tag;
    uns     ripctr;
    uns64   last_access;
    Addr    orig_lineaddr;
    uns     bank;      // DRAM bank of the line (set on install when bank_of is set)
    Flag    cleansed;  // BARD-C wrote it back and it has not been written again since
};

// BARD (Bank-Aware Replacement Decisions, Vittal & Qureshi, HPCA 2026)
typedef enum MCache_Bard_Enum {
    BARD_OFF=0,
    BARD_E=1,   // evict: dirty LRU victim to a bank with a pending write -> evict a dirty line to an idle bank
    BARD_C=2,   // cleanse: clean LRU victim -> also write back a dirty line to an idle bank, keep it (clean)
    BARD_H=3,   // hybrid: BARD-E when the LRU victim is dirty, BARD-C when it is clean
} MCache_Bard;

#define MCACHE_MAX_ASSOC 64


struct MCache{
  uns sets;
  uns assocs;
  MCache_ReplPolicy repl_policy; //0:LRU  1:RND 2:SRRIP
  uns index_policy; // how to index cache


  MCache_Entry *entries;
  int touched_wayid;
  int touched_setid;
  int touched_lineid;

  Flag evicted_dirty_line;
  Addr evicted_line_addr;
  Flag cleansed_line;       // BARD-C: this line was written back and stays cached (clean)
  Addr cleansed_line_addr;

  uns64 s_count; // number of accesses (also the LRU clock: never reset)
  uns64 s_count_base; // s_count at the last stats reset
  uns64 s_miss; // number of misses
  uns64 s_evict; // number of evictions
  uns64 s_writeback; // evictions of dirty lines (writebacks to memory)

  // BARD. BLP-Tracker: one bit per DRAM bank, set when the LLC writes back a
  // line to that bank (= the bank probably has a pending write); once all
  // bits of a sub-channel are set, that sub-channel's bits are cleared.
  MCache_Bard bard;
  uns   (*bank_of)(Addr lineaddr); // DRAM bank of a line, 0..bard_num_banks-1
  uns   bard_num_banks;
  uns   bard_banks_per_subch;
  uns64 bard_tracker;
  FILE  *bard_log;          // -bardlog: every eviction decision, for SCRIPTS/check_bard.py
  uns64 bard_log_left;

  // every eviction is exactly one of these three (BARD Fig. 10 breakdown)
  uns64 s_lru_evict;        // the LRU line, nothing cleansed
  uns64 s_bard_override;    // BARD-E: evicted a dirty line other than the LRU victim
  uns64 s_bard_cleanse;     // BARD-C: LRU line evicted and another dirty line written back
                            // (cleanses are DRAM writes too, not counted in s_writeback)
  uns64 s_cleanse_rewritten; // cleansed lines written again while cached: each needs
                             // one more writeback later (BARD-C's extra writes)
  uns64 s_tracker_reset;    // BLP-Tracker sub-channel self-resets
};

///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////

MCache *mcache_new(uns sets, uns assocs, uns repl );
Flag    mcache_access        (MCache *c, Addr addr);
void    mcache_install       (MCache *c, Addr addr);
Flag    mcache_probe         (MCache *c, Addr addr);
Flag    mcache_invalidate    (MCache *c, Addr addr);
Flag    mcache_mark_dirty    (MCache *c, Addr addr);
uns     mcache_get_index     (MCache *c, Addr addr);


uns     mcache_find_victim   (MCache *c, uns set);
uns     mcache_find_victim_lru   (MCache *c, uns set);
uns     mcache_find_victim_rnd   (MCache *c, uns set);
uns     mcache_find_victim_srrip   (MCache *c, uns set);

void    mcache_print_stats(MCache *c, char *header);
void    mcache_reset_stats(MCache *c); // end of warmup: zero the counters, keep the contents

void    mcache_bard_init(MCache *c, uns bard, uns (*bank_of)(Addr lineaddr), uns num_banks, uns banks_per_subch);
void    mcache_bard_log(MCache *c, const char *fname, uns64 max_events);
///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////

#endif // MCACHE_H
