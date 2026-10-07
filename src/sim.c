 /*************************************************************************
 * File         : sim.c
 * Author       : Moinuddin Qureshi
 * Description  : Fast Memory System  Simulator
 *************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <string>

#include "global_types.h"
#include "mcore.h"
#include "memsys.h"
#include "params.h"



/***************************************************************************
 * Globals 
 **************************************************************************/

MemSys      *memsys; 
OS          *os;
MCache      *LLC;
MCore       *mcore[MAX_THREADS];



/***************************************************************************************
 * Functions
 ***************************************************************************************/

void print_dots()
{
  uns ii;
  uns LINE_INTERVAL = 50 *  DOT_INTERVAL;

  last_printdot_cycle = cycle;

  if(TRACE_LIMIT){
    /* if(memsys->s_totaccess >= TRACE_LIMIT){
	  for(ii=0; ii< NUM_THREADS; ii++){
	      MCore *c=mcore[ii];
	      c->done_inst_count  = c->inst_num;
	      c->done_cycle_count = c->cycle;
	      c->done_miss_count  = c->miss_count;
	      c->done = 1;
	  }
	  }*/
  }
  
  if(!PRINT_DOTS){
      return;
  }

  if (cycle % LINE_INTERVAL ==0){
	printf("\n%4llu M\t", cycle/1000000);

	printf("(INST: ");
	for(ii=0; ii<NUM_THREADS; ii++){
	  uns my_inst = (uns)(mcore[ii]->inst_num);
	  printf("%5u M,", my_inst/1000000);
	  //printf("%9u ,", my_inst);
	}
	printf(") ");
	//memsys_print_state(memsys);
	fflush(stdout);

	for(ii=0; ii<NUM_THREADS; ii++){
	  //mcore_print_state(mcore[ii]);
	}
    }
    else{
	printf(".");
	fflush(stdout);
    }
    
}


/***************************************************************************************
 * Statistics report: the main output at the end, and the warmup file
 ***************************************************************************************/

static void print_report(Flag end_of_run, uns64 sys_cycles)
{
  int ii;

  printf("\n\n");
   
  printf("------------------------------------\n");
  printf("------  MEMSIM.V1.1  ----------\n");
  printf("------------------------------------\n");

  for(ii=0; ii<num_threads; ii++){
    if(end_of_run) mcore_print_stats(mcore[ii]);   // also closes the trace
    else           mcore_print_counters(mcore[ii]);
  }

  mcache_print_stats(LLC, (char*) "L3CACHE");
  if(end_of_run) memsys_print_stats(memsys);
  else           memsys_print_snapshot(memsys);

  os_print_stats(os);

  if(WARMUP_INST && end_of_run){
    printf("\nWARMUP_INST     \t : %llu", WARMUP_INST);
    printf("\nWARMUP_CYCLES   \t : %llu", warmup_end_cycle);
    printf("\nWARMUP_FILE     \t : %s", WARMUP_FILE);
    printf("\n(stats above cover only the measured instructions after warmup; OS_PAGE_MISS covers the whole run)\n");
  }
  
  uns64 sum_done_cycles=0;
  for(ii=0; ii<num_threads; ii++){
    sum_done_cycles+=(uns64)(mcore[ii]->done_cycle_count);
  }
  uns64 avg_done_cycles=(uns64)(sum_done_cycles)/(uns64)(num_threads);

  printf("\nSYS_CYCLES      \t : %llu", sys_cycles);
  printf("\nAVG_CORE_CYCLES \t : %llu", avg_done_cycles);

  printf("\n\n\n");
}

/***************************************************************************************
 * End of warmup (all cores executed WARMUP_INST instructions): write the stats so
 * far to WARMUP_FILE, then reset every counter so the main output covers only the
 * measured instructions. Machine state (cache contents, DRAM rows and queues,
 * page mappings) is kept: that is what the warmup is for.
 ***************************************************************************************/

static void end_warmup(void)
{
  int ii;
  warmup_end_cycle = cycle + 1;  // called at the end of this cycle

  if(WARMUP_FILE[0] == '\0'){
    std::string name = addr_trace_filename[0];
    name = name.substr(name.find_last_of('/') + 1);
    for(const char *ext : {".gz", ".mtf"}){
      size_t len = strlen(ext);
      if(name.size() > len && name.compare(name.size() - len, len, ext) == 0) name.resize(name.size() - len);
    }
    snprintf(WARMUP_FILE, sizeof(WARMUP_FILE), "output/%s_warmup.txt", name.c_str());
  }

  for(ii=0; ii<num_threads; ii++){
    mcore_snapshot_stats(mcore[ii]);
  }

  fflush(stdout);
  std::cout.flush();
  int fd = open(WARMUP_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd < 0){
    fprintf(stderr, "\nWARNING: cannot write the warmup stats to %s\n", WARMUP_FILE);
  }else{
    int saved_stdout = dup(1);
    dup2(fd, 1);
    close(fd);
    printf("MEMSIM WARMUP STATS: the first %llu instructions per core (%llu cycles),\n", WARMUP_INST, warmup_end_cycle);
    printf("excluded from the main results.\n");
    print_report(FALSE, warmup_end_cycle);
    fflush(stdout);
    std::cout.flush();
    dup2(saved_stdout, 1);
    close(saved_stdout);
  }

  for(ii=0; ii<num_threads; ii++){
    mcore_reset_stats(mcore[ii]);
  }
  mcache_reset_stats(LLC);
  memsys_reset_stats(memsys);
  warmup_done = 1;

  printf("\nWarmup done at cycle %llu: stats reset (warmup stats in %s)\n", warmup_end_cycle, WARMUP_FILE);
  fflush(stdout);
}


/***************************************************************************************
 * Main
 ***************************************************************************************/
int main(int argc, char** argv)
{
  int   ii;
  Flag  all_cores_done=0;
  
  if (argc < 2) {
    die_usage();
  }
  
  read_params(argc, argv);
  
  //--------------------------------------------------------------------
  // -- Allocate the nest and cores
  //--------------------------------------------------------------------
  uns num_os_pages = (1024*(MEM_SIZE_MB-MEM_RSRV_MB))/(OS_PAGESIZE/1024);
  os = os_new(num_os_pages, num_threads);

  uns l3sets = (L3_SIZE_KB*1024)/(L3_ASSOC*LINESIZE);

  memsys = memsys_new();
  if(CHECKMAP_LINES){
    memsys_check_map(memsys, CHECKMAP_LINES);
    exit(0);
  }
  LLC = mcache_new(l3sets, L3_ASSOC, L3_REPL);

  for(ii=0; ii<num_threads; ii++){
    mcore[ii] = mcore_new( memsys, os, LLC, addr_trace_filename[ii], ii);
  }
  
  srand(RAND_SEED);
  print_dots();

  //--------------------------------------------------------------------
  // -- Iterate through the traces by cycling all cores till done
  //--------------------------------------------------------------------
  
  while( ! (all_cores_done) ){
    all_cores_done=1;

    for(ii=0; ii<num_threads; ii++){
      mcore_cycle(mcore[ii]);
      all_cores_done &= mcore[ii]->done;
    }

    memsys_cycle(memsys); // every cycle, cycle through DRAM

    if(WARMUP_INST && !warmup_done){
      Flag all_warm = TRUE;
      for(ii=0; ii<num_threads; ii++){
	all_warm &= (mcore_lifetime_inst(mcore[ii]) >= WARMUP_INST);
      }
      if(all_warm){
	end_warmup();
	if(INST_LIMIT == 0){
	  break;  // warmup-only run: everything is in the warmup file
	}
      }
    }
    if (cycle - last_printdot_cycle >= DOT_INTERVAL){
      print_dots();
    }
      
    cycle++;
  }
    
  //--------------------------------------------------------------------
  // -- Print statistics
  //--------------------------------------------------------------------

  if(WARMUP_INST && INST_LIMIT == 0){
    memsys_finalize(memsys);  // close Ramulator2's log files
    printf("\nWarmup-only run (-inst_limit 0): all statistics are in %s\n\n", WARMUP_FILE);
    return 0;
  }

  print_report(TRUE, WARMUP_INST ? cycle - warmup_end_cycle : cycle);

  return 0;
}






