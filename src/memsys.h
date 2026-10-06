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
void    memsys_print_stats(MemSys *m);
void    memsys_print_dram_timings(MemSys *m);

///////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////

#endif // MEMSYS_H
