#ifndef DRAMCHANNEL_H
#define DRAMCHANNEL_H

#include "global_types.h"
#include "drambank.h"

#define NUM_DRAM_BUSQ_ENTRIES 128
#define NUM_DRAM_RDRETQ_ENTRIES 256 // >= tCAS+tBUS+1 (at most one RD issued per cycle)
#define NUM_DRAM_WRQ_MAX 256        // upper bound on MEM_WRQ_SIZE


typedef struct DRAM_Channel   DRAM_Channel;
typedef struct DRAM_BusQ DRAM_BusQ;
typedef struct DRAM_RdRetQ DRAM_RdRetQ;
typedef struct DRAM_BusQ_Entry DRAM_BusQ_Entry;
typedef struct DRAM_WrQ DRAM_WrQ;
typedef struct DRAM_WrQ_Entry DRAM_WrQ_Entry;

typedef struct DRAM_TFAW_Token DRAM_TFAW_Token;
typedef struct DRAM_RDWR_Token DRAM_RDWR_Token;

/////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////

struct DRAM_BusQ_Entry{
  Flag valid;
  DRAM_ReqType reqtype;
  uns64 lineaddr; // to wake up
  uns64 ready_time; // when will it be ready for going on bus

  // Ramulator2 keeps a request in its read/write buffer until its ACT issues
  // (or, for a row hit, until its RD/WR issues). These track that point.
  Flag  leave_at_dispatch; // row hit: leaves the buffer when the column command issues
  uns64 leave_time;        // otherwise: the cycle its ACT issues
  int   wrq_index;         // for writes: its write queue entry
};


struct DRAM_BusQ{
  DRAM_BusQ_Entry entries[NUM_DRAM_BUSQ_ENTRIES];
  uns size; //  num valid entries
};


// reads issued on the bus, waiting tCAS+tBUS for data to return to the core
// FIFO is exact: every read gets the same latency and issue order is monotonic
struct DRAM_RdRetQ{
  Addr  lineaddr[NUM_DRAM_RDRETQ_ENTRIES];
  uns64 done_time[NUM_DRAM_RDRETQ_ENTRIES];
  uns   head; // oldest entry
  uns   size; // num valid entries
};


// per-channel write queue, modeled on Ramulator2's controller write buffer:
// writes wait here until the channel enters write mode (watermarks)
struct DRAM_WrQ_Entry{
  Flag  valid;
  Addr  lineaddr;
  uns   bankid;
  uns64 rowid;
  uns64 arrive;
  Flag  in_service;        // picked by its bank; still counts until it leaves
  Flag  leave_at_dispatch; // row hit: leaves when the WR issues
  uns64 leave_time;        // otherwise: leaves when its ACT issues
};

struct DRAM_WrQ{
  DRAM_WrQ_Entry entries[NUM_DRAM_WRQ_MAX];
  uns size;          // num valid entries (waiting + in service)
  uns n_inservice;   // entries in service, waiting to leave
  uns per_bank[32];  // waiting (not in service) entries per bank
};


struct DRAM_TFAW_Token {
  uns64     tfaw_time; // TFAW window
  uns64     prev_time[4]; // last 4 activates
};

struct DRAM_RDWR_Token {
  uns64     prev_time;        // last RD dispatch cycle on this channel
  uns64     prev_bank;        // bank of last RD dispatch
  Flag      has_prev;         // FALSE until the first RD dispatch
  uns64     bg_prev_time[32]; // last RD dispatch cycle per bank group
  Flag      bg_has_prev[32];  // FALSE until the first RD dispatch in that bank group

  uns64     prev_wr_time;       // last WR dispatch cycle on this channel
  Flag      has_prev_wr;        // FALSE until the first WR dispatch
  uns64     bank_wr_time[32];   // last WR dispatch cycle per bank
  Flag      bank_has_wr[32];    // FALSE until the first WR dispatch to that bank
};


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

struct DRAM_Channel {
    uns         id; // we will have multiple dram channels, so id helps differentiate
    char name[256];

    Flag       RFMAB;
    uns        RFMSB; //-- bit vector for RFMs


    DRAM_Bank   *bank[32]; // max of 32 banks per channel
    DRAM_BusQ   dbusq; // DRAM bus queue
    DRAM_RdRetQ rdretq; // reads in flight on the bus (tCAS+tBUS)
    DRAM_RdRetQ fwdq;   // reads served from the write queue (1 cycle)
    DRAM_WrQ    wrq;    // write queue

    Flag        write_mode;    // TRUE while draining the write queue
    uns64       reads_waiting; // reads in bank queues not yet serviced

    DRAM_TFAW_Token  tfaw_token;
    DRAM_RDWR_Token  rdwr_token;

    uns64       num_banks;
    uns64       num_rows;
    uns64       banks_in_bankgroup;
    uns64       s_RFM; // stats
    uns64       s_bus_time;
    uns64       s_wr_issued;      // writes sent to DRAM
    uns64       s_wr_coalesced;   // writes absorbed by a pending write to the same line
    uns64       s_rd_forwarded;   // reads served from the write queue
    uns64       s_wrq_full;       // writes refused because the write queue was full
    uns64       s_wr_drains;      // read-mode -> write-mode switches
    uns64       s_wr_mode_cycles; // cycles spent in write mode
};

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

DRAM_Channel*   dram_channel_new(uns id, uns num_banks, uns num_rows);
void    dram_channel_cycle(DRAM_Channel *c);

uns     dram_channel_insert(DRAM_Channel *c, uns bankid,  DRAM_ReqType type, uns64 rowid, Addr lineaddr);

void   dram_channel_insert_rdwrq(DRAM_Channel *c, uns bankid, DRAM_ReqType reqtype, Addr lineaddr, uns64 ready_time);
void   dram_channel_schedule_rdwrq(DRAM_Channel *c);
void   dram_channel_return_reads(DRAM_Channel *c);

Flag   dram_channel_insert_wr(DRAM_Channel *c, uns bankid, uns64 rowid, Addr lineaddr);
Flag   dram_channel_forward_rd(DRAM_Channel *c, Addr lineaddr);
int    dram_channel_pick_wr(DRAM_Channel *c, DRAM_Bank *b);
void   dram_channel_start_wr(DRAM_Channel *c, uns index, Flag opened, uns64 act_time);
void   dram_channel_retire_wr(DRAM_Channel *c, uns index);
void   dram_channel_purge_wrq(DRAM_Channel *c);
uns64  dram_channel_reads_in_buffer(DRAM_Channel *c);
void   dram_channel_set_write_mode(DRAM_Channel *c);


Flag   dram_channel_get_tfaw_token(DRAM_Channel *c, uns64 in_cycle);
Flag   dram_channel_get_rdwr_token(DRAM_Channel *c, uns64 in_cycle, uns64 my_bank, DRAM_ReqType type, uns64 *out_delay);

void   dram_channel_print_state(DRAM_Channel *c);


////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////


#endif // DRAMCHANNEL_H
