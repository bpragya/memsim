#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <deque>
#include <iostream>
#include <random>

#include "externs.h"
#include "memsys.h"
#include "mcore.h"

#include "ramulator/base/config.h"
#include "ramulator/base/factory.h"
#include "ramulator/base/request.h"
#include "ramulator/controller/controller_base.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/channel_mapper/i_channel_mapper.h"
#include "ramulator/memory_system/i_memory_system.h"

extern MCore *mcore[MAX_THREADS];
extern uns64  LINESIZE;
extern char   RAMULATOR_CONFIG_PATH[1024];

// Ramulator2 keeps an Implementation's children protected and has no public
// accessor for its controllers or channel mapper. A pointer-to-member taken
// inside a derived class is the standard way to read a protected member
// without changing Ramulator2 itself.
struct ImplChildren : Ramulator::Implementation {
  static std::vector<std::unique_ptr<Ramulator::Implementation>>& of(Ramulator::Implementation *impl){
    return impl->*(&ImplChildren::m_children);
  }
};

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

#define MEMSIM_CYCLE_NS 0.25  // memsim models a 4 GHz core

struct MemSys {
  Ramulator::IFrontEnd     *frontend;
  Ramulator::IMemorySystem *memory_system;

  // Ramulator2's own mapping components, so memsim can ask which bank a
  // line goes to exactly as the DRAM will map it (memsys_bank_of)
  Ramulator::IChannelMapper                *chmap;
  std::vector<Ramulator::ControllerBase*>  ctrls;  // one per channel, in channel order
  uns    banks_per_channel;

  // Clock domains, as in Ramulator2's own driver: per memsim cycle the DRAM
  // ticks mem_ratio/fe_ratio times (DDR5-4800: 3 ticks per 5 memsim cycles).
  int    fe_ratio;
  int    mem_ratio;
  int    clk_acc;

  // Writebacks Ramulator2's write queue refused; retried every cycle so no
  // writeback is ever dropped. When full, new reads are refused instead.
  struct PendingWB { Addr lineaddr; uns coreid; };
  std::deque<PendingWB> wbbuf;
  uns64  outstanding_reads;  // LLC misses in flight (MSHRs in use)

  // Cores refused because MSHRs or wbbuf were full, first come first served:
  // when space frees, the core at the front gets it. Without this, cores
  // that retry earlier in the cycle loop could starve a later core forever.
  std::deque<uns> waitq;
  uns64  s_wait_turn;        // reads refused because an earlier core was waiting

  uns64  s_wb_sent;          // writebacks from the LLC
  uns64  s_wb_delayed;       // ...that had to wait in wbbuf
  uns64  s_wb_stalls;        // reads refused because wbbuf was full
  uns64  s_wbbuf_max;        // peak wbbuf occupancy
  uns64  s_mshr_full;        // reads refused because all MSHRs were busy
  uns64  s_mshr_max;         // peak outstanding reads
};

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

MemSys *memsys_new(void){
  MemSys *m = new MemSys;

  Ramulator::ConfigNode config = Ramulator::Config::parse_config_file(RAMULATOR_CONFIG_PATH);

  // Tell Ramulator2 how many cores send requests (source_id = core id), so
  // its per-core stats are sized for -ratemode / multiple traces.
  Ramulator::ConfigNode::Map root = config.map();
  Ramulator::ConfigNode::Map frontend = root["frontend"].map();
  frontend["num_cores"] = Ramulator::ConfigNode(num_threads);
  root["frontend"] = Ramulator::ConfigNode(frontend);
  config = Ramulator::ConfigNode(root);

  m->frontend      = Ramulator::Factory::create_frontend(config);
  m->memory_system = Ramulator::Factory::create_memory_system(config);

  m->frontend->connect_memory_system(m->memory_system);
  m->memory_system->connect_frontend(m->frontend);

  m->chmap = nullptr;
  for(auto &child : ImplChildren::of(dynamic_cast<Ramulator::Implementation*>(m->memory_system))){
    if(auto *cm = dynamic_cast<Ramulator::IChannelMapper*>(child.get())) m->chmap = cm;
    if(auto *ct = dynamic_cast<Ramulator::ControllerBase*>(child.get())) m->ctrls.push_back(ct);
  }
  assert(m->chmap != nullptr && !m->ctrls.empty());
  m->banks_per_channel = m->ctrls[0]->m_device.m_bank_nodes.size();

  m->fe_ratio  = m->frontend->get_clock_ratio();
  m->mem_ratio = m->memory_system->get_clock_ratio();
  m->clk_acc   = 0;

  m->outstanding_reads = 0;
  m->s_wb_sent = m->s_wb_delayed = m->s_wb_stalls = m->s_wbbuf_max = 0;
  m->s_mshr_full = m->s_mshr_max = 0;
  m->s_wait_turn = 0;

  memsys_print_dram_timings(m);
  return m;
}

//////////////////////////////////////////////////////////////////////////
// Print the DRAM timings Ramulator2 is actually using, next to the BARD
// paper's Table I (DDR5-4800B x4), so the configuration can be checked.
//////////////////////////////////////////////////////////////////////////

static Ramulator::DRAMSpec* memsys_dram_spec(MemSys *m){
  return m->ctrls[0]->m_device.m_spec; // all channels share one spec
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

  // DRAM cycle length implied by the clock ratio; must match tCK, otherwise
  // the timings above are right in cycles but wrong in real time
  double dram_cycle_ns = MEMSIM_CYCLE_NS * m->fe_ratio / m->mem_ratio;
  printf("  Clock: %d DRAM cycle(s) per %d memsim cycle(s) = %.3f ns per DRAM cycle (memsim cycle %.2f ns)%s\n",
	 m->mem_ratio, m->fe_ratio, dram_cycle_ns, MEMSIM_CYCLE_NS,
	 (fabs(dram_cycle_ns - tck) <= 0.01 * tck) ? "" : "   WARNING: does not match tCK");

  // Geometry: channels = controllers; per-channel levels from the spec
  const auto &sizes = spec->organization.level_sizes;
  auto level = [&](const char *name){ return spec->has_level(name) ? sizes[spec->get_level_id(name)] : 1; };
  int ranks = level("Rank"), bgs = level("BankGroup"), banks = level("Bank");
  int nrows = level("Row"), ncols = level("Column");
  double gb = (double)m->ctrls.size() * ranks * bgs * banks * nrows * ncols * (spec->channel_width / 8) / (1 << 30);
  printf("  Geometry: %d channel(s) x %d rank(s) x %d bank groups x %d banks = %u banks/channel, %u total;"
	 " %d rows x %d columns, x%d chips; %.0f GB\n",
	 (int)m->ctrls.size(), ranks, bgs, banks, m->banks_per_channel, memsys_num_banks(m),
	 nrows, ncols, spec->organization.dq, gb);
  printf("  Address mapping: %s\n",
	 dynamic_cast<Ramulator::Implementation*>(m->ctrls[0]->m_addr_mapper)->get_name().c_str());
  printf("\n");
}

//////////////////////////////////////////////////////////////////////////
// Which DRAM bank a line maps to, using Ramulator2's own channel mapper and
// address mapper. Banks are numbered 0..memsys_num_banks()-1, channel by
// channel: channel * banks_per_channel + bank-in-channel.
//////////////////////////////////////////////////////////////////////////

static void memsys_map_line(MemSys *m, Addr lineaddr, Ramulator::Request &req){
  m->chmap->apply(req);
  m->ctrls[req.addr_vec[0]]->m_addr_mapper->apply(req);
}

uns   memsys_bank_of(MemSys *m, Addr lineaddr){
  Ramulator::Request req((Ramulator::Addr_t)(lineaddr * LINESIZE), Ramulator::Request::Type::Read);
  memsys_map_line(m, lineaddr, req);
  int ch = req.addr_vec[0];
  return ch * m->banks_per_channel + m->ctrls[ch]->m_device.get_flat_bank_id(req.addr_vec);
}

uns   memsys_num_banks(MemSys *m){
  return m->ctrls.size() * m->banks_per_channel;
}

uns   memsys_banks_per_channel(MemSys *m){
  return m->banks_per_channel;
}

//////////////////////////////////////////////////////////////////////////
// -checkmap N: map N line addresses (the first 4096 consecutive lines, then
// random lines over all of memory) and print each one's address vector and
// bank number, then a per-bank summary. Output can be checked with
// SCRIPTS/check_mapping/zen_ref.py.
//////////////////////////////////////////////////////////////////////////

void  memsys_check_map(MemSys *m, uns64 num_lines){
  Ramulator::DRAMSpec *spec = memsys_dram_spec(m);
  uns64 lines_in_mem = (MEM_SIZE_MB << 20) / LINESIZE;
  std::mt19937_64 rng(RAND_SEED);
  std::vector<uns64> per_bank(memsys_num_banks(m), 0);

  printf("# levels:");
  for(auto &name : spec->level_names) printf(" %s", name.c_str());
  printf(" FlatBank\n");
  for(uns64 i = 0; i < num_lines; i++){
    Addr line = (i < 4096) ? i : rng() % lines_in_mem;
    Ramulator::Request req((Ramulator::Addr_t)(line * LINESIZE), Ramulator::Request::Type::Read);
    memsys_map_line(m, line, req);
    uns bank = memsys_bank_of(m, line);
    per_bank[bank]++;
    printf("%llu", (uns64)(line * LINESIZE));
    for(auto v : req.addr_vec) printf(" %d", v);
    printf(" %u\n", bank);
  }

  for(uns ch = 0; ch < m->ctrls.size(); ch++){
    uns used = 0;
    uns64 lo = ~0ULL, hi = 0;
    for(uns b = ch * m->banks_per_channel; b < (ch + 1) * m->banks_per_channel; b++){
      used += (per_bank[b] > 0);
      lo = std::min(lo, per_bank[b]);
      hi = std::max(hi, per_bank[b]);
    }
    printf("# channel %u: %u of %u banks used, lines per bank min %llu max %llu\n",
	   ch, used, m->banks_per_channel, lo, hi);
  }
}


//////////////////////////////////////////////////////////////////////////
// NOTE: ACCESSES TO THE MEMORY USE LINEADDR OF THE CACHELINE ACCESSED
// (i.e. byte_addr / LINESIZE) -- Ramulator2 wants byte addresses, so we
// scale by LINESIZE at this boundary.
//////////////////////////////////////////////////////////////////////////

Flag memsys_access(MemSys *m, Addr lineaddr, uns coreid, uns robid, uns64 inst_num){

  // back-pressure: the core sleeps and retries this read later
  bool wb_full   = (m->wbbuf.size() >= MEM_WBBUF_SIZE);
  bool mshr_full = (LLC_MSHRS && m->outstanding_reads >= LLC_MSHRS);
  bool waiting   = std::find(m->waitq.begin(), m->waitq.end(), coreid) != m->waitq.end();
  if(wb_full || mshr_full){
    if(wb_full)   m->s_wb_stalls++;
    if(mshr_full) m->s_mshr_full++;
    if(!waiting)  m->waitq.push_back(coreid);
    return FALSE;
  }
  if(!m->waitq.empty() && m->waitq.front() != coreid){
    m->s_wait_turn++;  // space is free, but an earlier-refused core goes first
    if(!waiting)  m->waitq.push_back(coreid);
    return FALSE;
  }

  auto on_read_done = [m, coreid, robid, inst_num](Ramulator::Request &req){
    m->outstanding_reads--;
    mcore_rob_wakeup(mcore[coreid], robid, inst_num);
  };

  bool accepted = m->frontend->receive_external_requests(
      Ramulator::Request::Type::Read,
      (Ramulator::Addr_t)(lineaddr * LINESIZE),
      (int)coreid,
      on_read_done,
      (int)LINESIZE);

  if(accepted){
    if(!m->waitq.empty() && m->waitq.front() == coreid){
      m->waitq.pop_front();
    }
    m->outstanding_reads++;
    if(m->outstanding_reads > m->s_mshr_max){
      m->s_mshr_max = m->outstanding_reads;
    }
  }
  return accepted ? TRUE : FALSE;
}

//////////////////////////////////////////////////////////////////////////
// Writebacks are never dropped: one Ramulator2's write queue refuses waits
// in wbbuf and is retried every cycle in memsys_cycle().
//////////////////////////////////////////////////////////////////////////

static bool memsys_send_writeback(MemSys *m, Addr lineaddr, uns coreid){
  auto on_wb_done = [](Ramulator::Request &req){};
  return m->frontend->receive_external_requests(
      Ramulator::Request::Type::Write,
      (Ramulator::Addr_t)(lineaddr * LINESIZE),
      (int)coreid,
      on_wb_done,
      (int)LINESIZE);
}

void  memsys_writeback(MemSys *m, Addr lineaddr, uns coreid){
  m->s_wb_sent++;
  if(!memsys_send_writeback(m, lineaddr, coreid)){
    m->wbbuf.push_back({lineaddr, coreid});
    m->s_wb_delayed++;
    if(m->wbbuf.size() > m->s_wbbuf_max){
      m->s_wbbuf_max = m->wbbuf.size();
    }
  }
}


//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  memsys_cycle(MemSys *m){
  m->clk_acc += m->mem_ratio;
  while(m->clk_acc >= m->fe_ratio){
    m->clk_acc -= m->fe_ratio;
    m->memory_system->tick();
  }

  // retry waiting writebacks, oldest first; a full write queue on one
  // channel does not hold back writebacks to the other
  for(auto it = m->wbbuf.begin(); it != m->wbbuf.end(); ){
    if(memsys_send_writeback(m, it->lineaddr, it->coreid)){
      it = m->wbbuf.erase(it);
    }else{
      ++it;
    }
  }
}

//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  memsys_print_stats(MemSys *m){
  // Same order as Ramulator2's own driver: finalize computes the averaged
  // stats (read latency, queue lengths, throughput) and closes plugin files.
  m->frontend->finalize();
  m->memory_system->finalize();
  memsys_print_counters(m);
}

//////////////////////////////////////////////////////////////////////////
// Mid-run report (end of warmup): compute Ramulator2's averages without
// finalize(), which would close its log files.
//////////////////////////////////////////////////////////////////////////

void  memsys_print_snapshot(MemSys *m){
  m->memory_system->update_stats_recursive();
  memsys_print_counters(m);
}

//////////////////////////////////////////////////////////////////////////
// End of warmup: zero memsim's and Ramulator2's counters. Queues, open
// rows, waiting writebacks and in-flight reads are untouched.
//////////////////////////////////////////////////////////////////////////

void  memsys_reset_stats(MemSys *m){
  m->s_wb_sent = m->s_wb_delayed = m->s_wb_stalls = 0;
  m->s_wbbuf_max = m->wbbuf.size();
  m->s_mshr_full = 0;
  m->s_mshr_max = m->outstanding_reads;
  m->s_wait_turn = 0;
  m->memory_system->reset_stats_recursive();
}

//////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////

void  memsys_print_counters(MemSys *m){
  printf("\nMEM_WB_SENT        \t : %llu", m->s_wb_sent);
  printf("\nMEM_WB_DELAYED     \t : %llu", m->s_wb_delayed);
  printf("\nMEM_WB_STALLS      \t : %llu", m->s_wb_stalls);
  printf("\nMEM_WBBUF_MAX      \t : %llu", m->s_wbbuf_max);
  printf("\nMEM_WBBUF_LEFT     \t : %llu", (uns64)m->wbbuf.size());
  printf("\nMEM_MSHR_FULL      \t : %llu", m->s_mshr_full);
  printf("\nMEM_MSHR_MAX       \t : %llu", m->s_mshr_max);
  printf("\nMEM_WAIT_TURN      \t : %llu", m->s_wait_turn);
  printf("\n");
  m->memory_system->print_stats(std::cout);
}
