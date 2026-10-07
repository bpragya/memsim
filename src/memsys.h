#ifndef MEMSYS_H
#define MEMSYS_H

#include "global_types.h"

typedef struct MemSys MemSys;

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

MemSys *memsys_new(void);
Flag    memsys_access(MemSys *m, Addr lineaddr, uns coreid, uns robid, uns64 inst_num);
void    memsys_writeback(MemSys *m, Addr lineaddr, uns coreid);
void    memsys_cycle(MemSys *m);
void    memsys_print_stats(MemSys *m);        // end of run (finalizes Ramulator2)
void    memsys_print_snapshot(MemSys *m);     // mid-run report (end of warmup)
void    memsys_print_counters(MemSys *m);
void    memsys_reset_stats(MemSys *m);        // end of warmup
void    memsys_finalize(MemSys *m);           // end of run: flush and close Ramulator2's files
void    memsys_print_dram_timings(MemSys *m);
uns     memsys_bank_of(MemSys *m, Addr lineaddr);   // DRAM bank of a line, 0..memsys_num_banks()-1
uns     memsys_num_banks(MemSys *m);
uns     memsys_banks_per_channel(MemSys *m);
void    memsys_check_map(MemSys *m, uns64 num_lines);

///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////

#endif // MEMSYS_H
