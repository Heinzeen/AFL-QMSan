/*
   american fuzzy lop++ - target execution related routines
   --------------------------------------------------------

   Originally written by Michal Zalewski

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com> and
                        Dominik Maier <mail@dmnk.co>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2024 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     https://www.apache.org/licenses/LICENSE-2.0

   This is the real deal: the program takes an instrumented binary and
   attempts a variety of basic fuzzing tricks, paying close attention to
   how they affect the execution path.

 */

#include "afl-fuzz.h"
#include <sys/time.h>
#include <signal.h>
#include <limits.h>
#if !defined NAME_MAX
  #define NAME_MAX _XOPEN_NAME_MAX
#endif

#include "cmplog.h"

#ifdef PROFILING
u64 time_spent_working = 0;
#endif


#ifdef QMSAN_STORE_TESTCASE
u64 runs_counter;
char * testcases;

#define QMSAN_DUMP_TESTCASE() \
        char *command = alloc_printf("cp %s/.cur_input %s/%llu", \
                        afl->out_dir, testcases, runs_counter);\
        system(command);\
        ck_free(command);
#endif


#ifdef QMSAN

#define valgrind_path   "valgrind --expensive-definedness-checks=yes \
                        --exit-on-first-error=yes  --error-exitcode=-1 "

char *accurate_log;

#define QMSAN_DUMP_TP() \
        char *command = alloc_printf("cp %s/.cur_input %s/%llu", \
                        afl->out_dir, true_positives, afl->heavyweight_runs);\
        system(command);\
        ck_free(command);

#ifdef QMSAN_DEBUG

#define QMSAN_LOG(x...) dprintf(afl->qmsan_log_fd, x); \
                              logged = 1;
#define QMSAN_DUMP_FP() \
        char *command = alloc_printf("cp %s/.cur_input %s/%llu", \
                        afl->out_dir, false_positives, afl->heavyweight_runs);\
        system(command);\
        ck_free(command);


#else
#define QMSAN_LOG(x...)             ;
#define QMSAN_DUMP_FP()             ;
#endif


#define QMSAN_AFL_UNTOUCHED             0
#define QMSAN_AFL_CLEAN                 1 << 0
#define QMSAN_AFL_ERROR                 1 << 1
#define QMSAN_AFL_MEMORY                1 << 2
#define QMSAN_AFL_PC_EDGE               1 << 3
#define QMSAN_AFL_CS                    1 << 4
#define QMSAN_AFL_CS_EDGE               1 << 5
#define QMSAN_CHECK_UNTOUCHED(arg)      ((arg & 3) == 0)  //first 2 bits
#define QMSAN_CHECK_CLEAN(arg)          (arg & QMSAN_AFL_CLEAN) 
#define QMSAN_CHECK_ERROR(arg)          (arg & QMSAN_AFL_ERROR) 
#define QMSAN_CHECK_MEMORY(arg)         (arg & QMSAN_AFL_MEMORY) 
#define QMSAN_CHECK_PC_EDGE(arg)        (arg & QMSAN_AFL_PC_EDGE) 
#define QMSAN_CHECK_CS(arg)             (arg & QMSAN_AFL_CS) 
#define QMSAN_CHECK_CS_EDGE(arg)        (arg & QMSAN_AFL_CS_EDGE) 

char cmdline[4096];
char *false_positives, *true_positives;
int logged = 0;
int valgrind_mode = 0;

int check_msan_trace(afl_state_t *afl, u8* trace){

  int i = MAP_SIZE -1, heavyweight_needed = 0;
#ifdef QMSAN_FLAGGING
  int violation = 0;
#endif
  while(i--){
    u8 new = trace[i];
    u8 old = afl->msan_traces[i];

  //in this mode, we just check for the byte to contain something
  //useful if we just want to consider one property (e.g. edges)
#ifdef QMSAN_SIMPLE
    //run qmsan-full if we find a new arch
    if(!old && new){
      QMSAN_LOG("new byte: 0x%x\n", i);
      afl->qmsan_errors++;
      afl->msan_traces[i] = 1;
      heavyweight_needed = 1;
    }

#endif


#ifdef QMSAN_NAIVE
    //run qmsan-full whenever qmsan-light finds a possible positive
    //no need to update our side of the map in this case, so we can just return
    if(QMSAN_CHECK_ERROR(new)){
      QMSAN_LOG("new intruction found: 0x%x\n", i);
      heavyweight_needed = 1;
      break;
    }
#endif

#ifdef QMSAN_FILTERING
    //run qmsan-full if we find a new buggy instruction
    if(!QMSAN_CHECK_ERROR(old) && QMSAN_CHECK_ERROR(new)){
      QMSAN_LOG("new faulty instruction: 0x%x\n", i);
      afl->msan_traces[i] |= QMSAN_AFL_ERROR;
      afl->qmsan_errors++;
      heavyweight_needed = 1;
    }
#endif

#ifdef QMSAN_DROPOFF
    //same as filtering, but implement a drop-off from 1 to 255 (only powers
    //of two). at 255 we stop checking
    if(QMSAN_CHECK_ERROR(new)){
      if(!old)
        afl->qmsan_errors++;
      if(old != 0xff){
        afl->msan_traces[i]++;
        //check if it is a power of two
        if ((old & (old - 1)) == 0)
          heavyweight_needed = 1;
      }
    }
#endif

#ifdef QMSAN_FLAGGING
    //instruction i raised an error
    //if(QMSAN_CHECK_ERROR(trace[i]))
    //    SAYF("trace[%d] = %hhx\n", i, trace[i]);
    if(QMSAN_CHECK_ERROR(new)){
      if(!violation){
        afl->qmsan_violations++;
        violation = 1;
      }
        //SAYF("[%d] macarena\n", i);

      if(QMSAN_CHECK_UNTOUCHED(old)){ //first time it raises an error
        QMSAN_LOG("[0x%x] raised an error\n", i);
        afl->msan_traces[i] |= QMSAN_AFL_ERROR;
        heavyweight_needed = 1;
        afl->qmsan_errors++;
      }

      else if(QMSAN_CHECK_CLEAN(afl->msan_traces[i])){
        QMSAN_LOG("[0x%x] mixed - raised an error\n", i);
        afl->msan_traces[i] |= QMSAN_AFL_ERROR;
        heavyweight_needed = 1;
      }
      //else
      //  SAYF("afl->msan_traces[%d] = %hhx\n", i, afl->msan_traces[i] & 3);
    }

    //manage the case when an instruction that was ignore listed
    //so far turns out to not always raise an error.
    else if(QMSAN_CHECK_CLEAN(new)
              && !QMSAN_CHECK_ERROR(new)
              && QMSAN_CHECK_ERROR(old)
              && !QMSAN_CHECK_CLEAN(old)){
      QMSAN_LOG("[0x%x] is mixed\n", i);
      afl->msan_traces[i] |= QMSAN_AFL_CLEAN;
      afl->qmsan_mixed++;
    }
#endif

#ifdef QMSAN_EDGES
    //run qmsan-full if we find a new arch
    if(!QMSAN_CHECK_PC_EDGE(old) && QMSAN_CHECK_PC_EDGE(new)){
      QMSAN_LOG("new edge: 0x%x\n", i);
      afl->qmsan_edges++;
      afl->msan_traces[i] |= QMSAN_AFL_PC_EDGE;
      heavyweight_needed = 1;
    }

#endif

#ifdef QMSAN_MEMORY
    //run qmsan-full if we find a new couple (instr, mem)
    if(!QMSAN_CHECK_MEMORY(old) && QMSAN_CHECK_MEMORY(new)){
      QMSAN_LOG("new (instr, memory): 0x%x\n", i);
      afl->qmsan_memory++;
      afl->msan_traces[i] |= QMSAN_AFL_MEMORY;
      heavyweight_needed = 1;
    }

#endif

#ifdef QMSAN_CALLSTACK
    //run qmsan-full if we find a new arch
    if(!QMSAN_CHECK_CS(old) && QMSAN_CHECK_CS(new)){
      QMSAN_LOG("new callstack entry: 0x%x\n", i);
      afl->qmsan_callstack++;
      afl->msan_traces[i] |= QMSAN_AFL_CS;
      heavyweight_needed = 1;
    }

#endif

#ifdef QMSAN_CALLSTACK_EDGES
    //run qmsan-full if we find a new arch
    if(!QMSAN_CHECK_CS_EDGE(old) && QMSAN_CHECK_CS_EDGE(new)){
      QMSAN_LOG("new callstack edge: 0x%x\n", i);
      afl->qmsan_callstack_edges++;
      afl->msan_traces[i] |= QMSAN_AFL_CS_EDGE;
      heavyweight_needed = 1;
    }

#endif
  }


  return heavyweight_needed;
}

//TODO: this should be moved in afl-fuzz.c and made a one-time thing.
//      Also, in general, we should not rely on .cur_inpuut, instead
//      we sahould make a generic cmdline with some fmt stuff and call it.
void init_cmdline(afl_state_t *afl){

    int i = 2;
    //if we are in ARM, we need to add the ld_prefix for its libraries
    char* ld_prefix = getenv("QEMU_LD_PREFIX");
    if(ld_prefix)
      sprintf(cmdline, "QEMU_LD_PREFIX=%s ", ld_prefix);
    char* path = getenv("QMSAN_PATH");
    if(path)
      OKF("Using QMSan's accurate detector");
    else{
      ACTF("Trying to use valgrind as accurate detector");
      //run valgrind --version to see if it is installed
      path = valgrind_path;
      char aux[256];
      sprintf(aux, "%s %s >%s 2>&1", valgrind_path, "--version", accurate_log);
      int status = system(aux);
      if(WEXITSTATUS(status)){
        BADF("Error while trying to invoke valgrind (exit code: %d)", status);
        exit(1);
      }
      OKF("Valgrind works fine, let's go!");
      valgrind_mode = 1;
    }
    sprintf(cmdline + strlen(cmdline), "%s ", path);
    while(afl->argv[i]){//that strlen sucks, but we only have to do this once
      sprintf(cmdline + strlen(cmdline), "%s ", afl->argv[i]);
      i++;
    }
    //redirect the output
    sprintf(cmdline + strlen(cmdline), " >>%s 2>&1 ", accurate_log);
    if(strlen(cmdline) > 4095)    //TODO: make a proper exit in such case
      BADF("Buffer overflow\n");
    QMSAN_LOG("%s\n", cmdline);

    //store qmsan false positives
    false_positives = alloc_printf("%s/%s", afl->out_dir, "qmsan_fp");
    mkdir(false_positives, 0777);

    //store qmsan true positives (sometimes AFL fails to recognize them)
    true_positives = alloc_printf("%s/%s", afl->out_dir, "qmsan_tp");
    mkdir(true_positives, 0777);
  
}
#endif


/* Execute target application, monitoring for timeouts. Return status
   information. The called program will update afl->fsrv->trace_bits. */

fsrv_run_result_t __attribute__((hot))
fuzz_run_target(afl_state_t *afl, afl_forkserver_t *fsrv, u32 timeout) {

#ifdef PROFILING
  static u64      time_spent_start = 0;
  struct timespec spec;
  if (time_spent_start) {

    u64 current;
    clock_gettime(CLOCK_REALTIME, &spec);
    current = (spec.tv_sec * 1000000000) + spec.tv_nsec;
    time_spent_working += (current - time_spent_start);

  }

#endif

  fsrv_run_result_t res = afl_fsrv_run_target(fsrv, timeout, &afl->stop_soon);

  /* If post_run() function is defined in custom mutator, the function will be
     called each time after AFL++ executes the target program. */

  if (unlikely(afl->custom_mutators_count)) {

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (unlikely(el->afl_custom_post_run)) {

        el->afl_custom_post_run(el->data);

      }

    });

  }

//protect it with ifdefs so it does not slow us down if we don't need it
#ifdef QMSAN_STORE_TESTCASE
if (getenv("QAFL_STORE_TESTCASES")) {
    if(runs_counter < strtoul(getenv("QAFL_STORE_TESTCASES"), NULL, 10)){
      if(!testcases){
        testcases = alloc_printf("%s/%s", afl->out_dir, "saved_testcases");
        mkdir(testcases, 0777);
      }
      QMSAN_DUMP_TESTCASE();

      runs_counter++;
    }
}
#endif

#ifdef QMSAN
  if(!cmdline[0]){
    init_cmdline(afl);
  }

  if(afl->fsrv.msan_bits && afl->fsrv.msan_bits[MAP_SIZE - 1]){
    if(check_msan_trace(afl, afl->fsrv.msan_bits)){
      afl->fsrv.msan_bits[MAP_SIZE - 1] = 0;
      //use heavyweight instrumentation
      QMSAN_LOG("Potential crash detected\n");
      int status = (int) WEXITSTATUS(system(cmdline));
      QMSAN_LOG("[system] exit status: %d\n", status);
      //we keep going since they will be at least saved with the FP
      //we can check them later
      afl->heavyweight_runs++;
      if((!valgrind_mode && afl->fsrv.msan_bits[MAP_SIZE - 1]) ||
          (valgrind_mode && status == 255)){
        afl->qmsan_crashes++;
        QMSAN_LOG("True positive [%llu]!!!.\n", afl->qmsan_crashes);
        res = FSRV_RUN_CRASH;
        afl->fsrv.msan_bits[MAP_SIZE - 1] = 0;
        QMSAN_DUMP_TP();
      }
      else{
        QMSAN_LOG("False positive [%llu]\n", afl->heavyweight_runs);
        QMSAN_DUMP_FP();
      }
      SAYF("\nmsan stats: [%llu/%llu]",
            afl->qmsan_crashes, afl->heavyweight_runs);
      QMSAN_LOG("msan stats: [%llu/%llu]\n",
            afl->qmsan_crashes, afl->heavyweight_runs);
    }
  }
#if defined QMSAN_FLAGGING || defined QMSAN_NAIVE
  //in this modes we need QEMU to have fresh bits in its map
  memset(afl->fsrv.msan_bits, QMSAN_AFL_UNTOUCHED, MAP_SIZE);
#endif
  if(logged){
    QMSAN_LOG("\n\n")
    logged = 0;
  }
#endif

#ifdef PROFILING
  clock_gettime(CLOCK_REALTIME, &spec);
  time_spent_start = (spec.tv_sec * 1000000000) + spec.tv_nsec;
#endif

  return res;

}

/* Write modified data to file for testing. If afl->fsrv.out_file is set, the
   old file is unlinked and a new one is created. Otherwise, afl->fsrv.out_fd is
   rewound and truncated. */

u32 __attribute__((hot))
write_to_testcase(afl_state_t *afl, void **mem, u32 len, u32 fix) {

  u8 sent = 0;

  if (unlikely(afl->custom_mutators_count)) {

    ssize_t new_size = len;
    u8     *new_mem = *mem;
    u8     *new_buf = NULL;

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_post_process) {

        new_size =
            el->afl_custom_post_process(el->data, new_mem, new_size, &new_buf);

        if (unlikely(!new_buf || new_size <= 0)) {

          new_size = 0;
          new_buf = new_mem;
          // FATAL("Custom_post_process failed (ret: %lu)", (long
          // unsigned)new_size);

        } else {

          new_mem = new_buf;

        }

      }

    });

    if (unlikely(!new_size)) {

      // perform dummy runs (fix = 1), but skip all others
      if (fix) {

        new_size = len;

      } else {

        return 0;

      }

    }

    if (unlikely(new_size < afl->min_length && !fix)) {

      new_size = afl->min_length;

    } else if (unlikely(new_size > afl->max_length)) {

      new_size = afl->max_length;

    }

    if (new_mem != *mem && new_mem != NULL && new_size > 0) {

      new_buf = afl_realloc(AFL_BUF_PARAM(out_scratch), new_size);
      if (unlikely(!new_buf)) { PFATAL("alloc"); }
      memcpy(new_buf, new_mem, new_size);

      /* if AFL_POST_PROCESS_KEEP_ORIGINAL is set then save the original memory
         prior post-processing in new_mem to restore it later */
      if (unlikely(afl->afl_env.afl_post_process_keep_original)) {

        new_mem = *mem;

      }

      *mem = new_buf;
      afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));

    }

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_fuzz_send) {

        el->afl_custom_fuzz_send(el->data, *mem, new_size);
        sent = 1;

      }

    });

    if (likely(!sent)) {

      /* everything as planned. use the potentially new data. */
      afl_fsrv_write_to_testcase(&afl->fsrv, *mem, new_size);

      if (likely(!afl->afl_env.afl_post_process_keep_original)) {

        len = new_size;

      } else {

        /* restore the original memory which was saved in new_mem */
        *mem = new_mem;
        afl_swap_bufs(AFL_BUF_PARAM(out), AFL_BUF_PARAM(out_scratch));

      }

    }

  } else {                                   /* !afl->custom_mutators_count */

    if (unlikely(len < afl->min_length && !fix)) {

      len = afl->min_length;

    } else if (unlikely(len > afl->max_length)) {

      len = afl->max_length;

    }

    /* boring uncustom. */
    afl_fsrv_write_to_testcase(&afl->fsrv, *mem, len);

  }

#ifdef _AFL_DOCUMENT_MUTATIONS
  s32  doc_fd;
  char fn[PATH_MAX];
  snprintf(fn, PATH_MAX, "%s/mutations/%09u:%s", afl->out_dir,
           afl->document_counter++,
           describe_op(afl, 0, NAME_MAX - strlen("000000000:")));

  if ((doc_fd = open(fn, O_WRONLY | O_CREAT | O_TRUNC, DEFAULT_PERMISSION)) >=
      0) {

    if (write(doc_fd, *mem, len) != len)
      PFATAL("write to mutation file failed: %s", fn);
    close(doc_fd);

  }

#endif

  return len;

}

/* The same, but with an adjustable gap. Used for trimming. */

static void write_with_gap(afl_state_t *afl, u8 *mem, u32 len, u32 skip_at,
                           u32 skip_len) {

  s32 fd = afl->fsrv.out_fd;
  u32 tail_len = len - skip_at - skip_len;

  /*
  This memory is used to carry out the post_processing(if present) after copying
  the testcase by removing the gaps. This can break though
  */
  u8 *mem_trimmed = afl_realloc(AFL_BUF_PARAM(out_scratch), len - skip_len + 1);
  if (unlikely(!mem_trimmed)) { PFATAL("alloc"); }

  ssize_t new_size = len - skip_len;
  u8     *new_mem = mem;

  bool post_process_skipped = true;

  if (unlikely(afl->custom_mutators_count)) {

    u8 *new_buf = NULL;
    new_mem = mem_trimmed;

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_post_process) {

        // We copy into the mem_trimmed only if we actually have custom mutators
        // *with* post_processing installed

        if (post_process_skipped) {

          if (skip_at) { memcpy(mem_trimmed, (u8 *)mem, skip_at); }

          if (tail_len) {

            memcpy(mem_trimmed + skip_at, (u8 *)mem + skip_at + skip_len,
                   tail_len);

          }

          post_process_skipped = false;

        }

        new_size =
            el->afl_custom_post_process(el->data, new_mem, new_size, &new_buf);

        if (unlikely(!new_buf && new_size <= 0)) {

          new_size = 0;
          new_buf = new_mem;
          // FATAL("Custom_post_process failed (ret: %lu)", (long
          // unsigned)new_size);

        } else {

          new_mem = new_buf;

        }

      }

    });

  }

  if (likely(afl->fsrv.use_shmem_fuzz)) {

    if (!post_process_skipped) {

      // If we did post_processing, copy directly from the new_mem buffer

      memcpy(afl->fsrv.shmem_fuzz, new_mem, new_size);

    } else {

      memcpy(afl->fsrv.shmem_fuzz, mem, skip_at);

      memcpy(afl->fsrv.shmem_fuzz + skip_at, mem + skip_at + skip_len,
             tail_len);

    }

    *afl->fsrv.shmem_fuzz_len = new_size;

#ifdef _DEBUG
    if (afl->debug) {

      fprintf(
          stderr, "FS crc: %16llx len: %u\n",
          hash64(afl->fsrv.shmem_fuzz, *afl->fsrv.shmem_fuzz_len, HASH_CONST),
          *afl->fsrv.shmem_fuzz_len);
      fprintf(stderr, "SHM :");
      for (u32 i = 0; i < *afl->fsrv.shmem_fuzz_len; i++)
        fprintf(stderr, "%02x", afl->fsrv.shmem_fuzz[i]);
      fprintf(stderr, "\nORIG:");
      for (u32 i = 0; i < *afl->fsrv.shmem_fuzz_len; i++)
        fprintf(stderr, "%02x", (u8)((u8 *)mem)[i]);
      fprintf(stderr, "\n");

    }

#endif

    return;

  } else if (unlikely(!afl->fsrv.use_stdin)) {

    if (unlikely(afl->no_unlink)) {

      fd = open(afl->fsrv.out_file, O_WRONLY | O_CREAT | O_TRUNC,
                DEFAULT_PERMISSION);

    } else {

      unlink(afl->fsrv.out_file);                         /* Ignore errors. */
      fd = open(afl->fsrv.out_file, O_WRONLY | O_CREAT | O_EXCL,
                DEFAULT_PERMISSION);

    }

    if (fd < 0) { PFATAL("Unable to create '%s'", afl->fsrv.out_file); }

  } else {

    lseek(fd, 0, SEEK_SET);

  }

  if (!post_process_skipped) {

    ck_write(fd, new_mem, new_size, afl->fsrv.out_file);

  } else {

    ck_write(fd, mem, skip_at, afl->fsrv.out_file);

    ck_write(fd, mem + skip_at + skip_len, tail_len, afl->fsrv.out_file);

  }

  if (afl->fsrv.use_stdin) {

    if (ftruncate(fd, new_size)) { PFATAL("ftruncate() failed"); }
    lseek(fd, 0, SEEK_SET);

  } else {

    close(fd);

  }

}

/* Calibrate a new test case. This is done when processing the input directory
   to warn about flaky or otherwise problematic test cases early on; and when
   new paths are discovered to detect variable behavior and so on. */

u8 calibrate_case(afl_state_t *afl, struct queue_entry *q, u8 *use_mem,
                  u32 handicap, u8 from_queue) {

  u8 fault = 0, new_bits = 0, var_detected = 0, hnb = 0,
     first_run = (q->exec_cksum == 0);
  u64 start_us, stop_us, diff_us;
  s32 old_sc = afl->stage_cur, old_sm = afl->stage_max;
  u32 use_tmout = afl->fsrv.exec_tmout;
  u8 *old_sn = afl->stage_name;

  if (unlikely(afl->shm.cmplog_mode)) { q->exec_cksum = 0; }

  /* Be a bit more generous about timeouts when resuming sessions, or when
     trying to calibrate already-added finds. This helps avoid trouble due
     to intermittent latency. */

  if (!from_queue || afl->resuming_fuzz) {

    use_tmout = MAX(afl->fsrv.exec_tmout + CAL_TMOUT_ADD,
                    afl->fsrv.exec_tmout * CAL_TMOUT_PERC / 100);

  }

  ++q->cal_failed;

  afl->stage_name = "calibration";
  afl->stage_max = afl->afl_env.afl_cal_fast ? CAL_CYCLES_FAST : CAL_CYCLES;
  if (getenv("QMSAN_CAL_ONCE")) {
    afl->stage_max = CAL_ONCE;
  }

  /* Make sure the forkserver is up before we do anything, and let's not
     count its spin-up time toward binary calibration. */

  if (!afl->fsrv.fsrv_pid) {

    if (afl->fsrv.cmplog_binary &&
        afl->fsrv.init_child_func != cmplog_exec_child) {

      FATAL("BUG in afl-fuzz detected. Cmplog mode not set correctly.");

    }

    afl_fsrv_start(&afl->fsrv, afl->argv, &afl->stop_soon,
                   afl->afl_env.afl_debug_child);

    if (afl->fsrv.support_shmem_fuzz && !afl->fsrv.use_shmem_fuzz) {

      afl_shm_deinit(afl->shm_fuzz);
      ck_free(afl->shm_fuzz);
      afl->shm_fuzz = NULL;
      afl->fsrv.support_shmem_fuzz = 0;
      afl->fsrv.shmem_fuzz = NULL;

    }

  }

  /* we need a dummy run if this is LTO + cmplog */
  if (unlikely(afl->shm.cmplog_mode)) {

    (void)write_to_testcase(afl, (void **)&use_mem, q->len, 1);

    fault = fuzz_run_target(afl, &afl->fsrv, use_tmout);

    /* afl->stop_soon is set by the handler for Ctrl+C. When it's pressed,
       we want to bail out quickly. */

    if (afl->stop_soon || fault != afl->crash_mode) { goto abort_calibration; }

    if (!afl->non_instrumented_mode && !afl->stage_cur &&
        !count_bytes(afl, afl->fsrv.trace_bits)) {

      fault = FSRV_RUN_NOINST;
      goto abort_calibration;

    }

#ifdef INTROSPECTION
    if (unlikely(!q->bitsmap_size)) q->bitsmap_size = afl->bitsmap_size;
#endif

  }

  if (q->exec_cksum) {

    memcpy(afl->first_trace, afl->fsrv.trace_bits, afl->fsrv.map_size);
    hnb = has_new_bits(afl, afl->virgin_bits);
    if (hnb > new_bits) { new_bits = hnb; }

  }

  start_us = get_cur_time_us();

  for (afl->stage_cur = 0; afl->stage_cur < afl->stage_max; ++afl->stage_cur) {

    if (unlikely(afl->debug)) {

      DEBUGF("calibration stage %d/%d\n", afl->stage_cur + 1, afl->stage_max);

    }

    u64 cksum;

    (void)write_to_testcase(afl, (void **)&use_mem, q->len, 1);

    fault = fuzz_run_target(afl, &afl->fsrv, use_tmout);

    /* afl->stop_soon is set by the handler for Ctrl+C. When it's pressed,
       we want to bail out quickly. */

    if (afl->stop_soon || fault != afl->crash_mode) { goto abort_calibration; }

    if (!afl->non_instrumented_mode && !afl->stage_cur &&
        !count_bytes(afl, afl->fsrv.trace_bits)) {

      fault = FSRV_RUN_NOINST;
      goto abort_calibration;

    }

#ifdef INTROSPECTION
    if (unlikely(!q->bitsmap_size)) q->bitsmap_size = afl->bitsmap_size;
#endif

    classify_counts(&afl->fsrv);
    cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);
    if (q->exec_cksum != cksum) {

      hnb = has_new_bits(afl, afl->virgin_bits);
      if (hnb > new_bits) { new_bits = hnb; }

      if (q->exec_cksum) {

        u32 i;

        for (i = 0; i < afl->fsrv.map_size; ++i) {

          if (unlikely(!afl->var_bytes[i]) &&
              unlikely(afl->first_trace[i] != afl->fsrv.trace_bits[i])) {

            afl->var_bytes[i] = 1;
            // ignore the variable edge by setting it to fully discovered
            afl->virgin_bits[i] = 0;

          }

        }

        if (unlikely(!var_detected && !afl->afl_env.afl_no_warn_instability)) {

          // note: from_queue seems to only be set during initialization
          if (afl->afl_env.afl_no_ui || from_queue) {

            WARNF("instability detected during calibration");

          } else if (afl->debug) {

            DEBUGF("instability detected during calibration\n");

          }

        }

        var_detected = 1;
        afl->stage_max =
            afl->afl_env.afl_cal_fast ? CAL_CYCLES : CAL_CYCLES_LONG;
        if (getenv("QMSAN_CAL_ONCE")) {
          afl->stage_max = CAL_ONCE;
        } 

      } else {

        q->exec_cksum = cksum;
        memcpy(afl->first_trace, afl->fsrv.trace_bits, afl->fsrv.map_size);

      }

    }

  }

  if (unlikely(afl->fixed_seed)) {

    diff_us = (u64)(afl->fsrv.exec_tmout - 1) * (u64)afl->stage_max;

  } else {

    stop_us = get_cur_time_us();
    diff_us = stop_us - start_us;
    if (unlikely(!diff_us)) { ++diff_us; }

  }

  afl->total_cal_us += diff_us;
  afl->total_cal_cycles += afl->stage_max;

  /* OK, let's collect some stats about the performance of this test case.
     This is used for fuzzing air time calculations in calculate_score(). */

  if (unlikely(!afl->stage_max)) {

    // Pretty sure this cannot happen, yet scan-build complains.
    FATAL("BUG: stage_max should not be 0 here! Please report this condition.");

  }

  q->exec_us = diff_us / afl->stage_max;
  q->bitmap_size = count_bytes(afl, afl->fsrv.trace_bits);
  q->handicap = handicap;
  q->cal_failed = 0;

  afl->total_bitmap_size += q->bitmap_size;
  ++afl->total_bitmap_entries;

  update_bitmap_score(afl, q);

  /* If this case didn't result in new output from the instrumentation, tell
     parent. This is a non-critical problem, but something to warn the user
     about. */

  if (!afl->non_instrumented_mode && first_run && !fault && !new_bits) {

    fault = FSRV_RUN_NOBITS;

  }

abort_calibration:

  if (new_bits == 2 && !q->has_new_cov) {

    q->has_new_cov = 1;
    ++afl->queued_with_cov;

  }

  /* Mark variable paths. */

  if (var_detected) {

    afl->var_byte_count = count_bytes(afl, afl->var_bytes);

    if (!q->var_behavior) {

      mark_as_variable(afl, q);
      ++afl->queued_variable;

    }

  }

  afl->stage_name = old_sn;
  afl->stage_cur = old_sc;
  afl->stage_max = old_sm;

  if (!first_run) { show_stats(afl); }

  return fault;

}

/* Grab interesting test cases from other fuzzers. */

void sync_fuzzers(afl_state_t *afl) {

  DIR           *sd;
  struct dirent *sd_ent;
  u32            sync_cnt = 0, synced = 0, entries = 0;
  u8             path[PATH_MAX + 1 + NAME_MAX];

  sd = opendir(afl->sync_dir);
  if (!sd) { PFATAL("Unable to open '%s'", afl->sync_dir); }

  afl->stage_max = afl->stage_cur = 0;
  afl->cur_depth = 0;

  /* Look at the entries created for every other fuzzer in the sync directory.
   */

  while ((sd_ent = readdir(sd))) {

    u8  qd_synced_path[PATH_MAX], qd_path[PATH_MAX];
    u32 min_accept = 0, next_min_accept = 0;

    s32 id_fd;

    /* Skip dot files and our own output directory. */

    if (sd_ent->d_name[0] == '.' || !strcmp(afl->sync_id, sd_ent->d_name)) {

      continue;

    }

    entries++;

    // secondary nodes only syncs from main, the main node syncs from everyone
    if (likely(afl->is_secondary_node)) {

      sprintf(qd_path, "%s/%s/is_main_node", afl->sync_dir, sd_ent->d_name);
      int res = access(qd_path, F_OK);
      if (unlikely(afl->is_main_node)) {  // an elected temporary main node

        if (likely(res == 0)) {  // there is another main node? downgrade.

          afl->is_main_node = 0;
          sprintf(qd_path, "%s/is_main_node", afl->out_dir);
          unlink(qd_path);

        }

      } else {

        if (likely(res != 0)) { continue; }

      }

    }

    synced++;

    /* document the attempt to sync to this instance */

    sprintf(qd_synced_path, "%s/.synced/%s.last", afl->out_dir, sd_ent->d_name);
    id_fd =
        open(qd_synced_path, O_RDWR | O_CREAT | O_TRUNC, DEFAULT_PERMISSION);
    if (id_fd >= 0) close(id_fd);

    /* Skip anything that doesn't have a queue/ subdirectory. */

    sprintf(qd_path, "%s/%s/queue", afl->sync_dir, sd_ent->d_name);

    struct dirent **namelist = NULL;
    int             m = 0, n, o;

    n = scandir(qd_path, &namelist, NULL, alphasort);

    if (n < 1) {

      if (namelist) free(namelist);
      continue;

    }

    /* Retrieve the ID of the last seen test case. */

    sprintf(qd_synced_path, "%s/.synced/%s", afl->out_dir, sd_ent->d_name);

    id_fd = open(qd_synced_path, O_RDWR | O_CREAT, DEFAULT_PERMISSION);

    if (id_fd < 0) { PFATAL("Unable to create '%s'", qd_synced_path); }

    if (read(id_fd, &min_accept, sizeof(u32)) == sizeof(u32)) {

      next_min_accept = min_accept;
      lseek(id_fd, 0, SEEK_SET);

    }

    /* Show stats */

    snprintf(afl->stage_name_buf, STAGE_BUF_SIZE, "sync %u", ++sync_cnt);

    afl->stage_name = afl->stage_name_buf;
    afl->stage_cur = 0;
    afl->stage_max = 0;

    /* For every file queued by this fuzzer, parse ID and see if we have
       looked at it before; exec a test case if not. */

    u8 entry[12];
    sprintf(entry, "id:%06u", next_min_accept);

    while (m < n) {

      if (strncmp(namelist[m]->d_name, entry, 9)) {

        m++;

      } else {

        break;

      }

    }

    if (m >= n) { goto close_sync; }  // nothing new

    for (o = m; o < n; o++) {

      s32         fd;
      struct stat st;

      snprintf(path, sizeof(path), "%s/%s", qd_path, namelist[o]->d_name);
      afl->syncing_case = next_min_accept;
      next_min_accept++;

      /* Allow this to fail in case the other fuzzer is resuming or so... */

      fd = open(path, O_RDONLY);

      if (fd < 0) { continue; }

      if (fstat(fd, &st)) { WARNF("fstat() failed"); }

      /* Ignore zero-sized or oversized files. */

      if (st.st_size && st.st_size <= MAX_FILE) {

        u8  fault;
        u8 *mem = mmap(0, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

        if (mem == MAP_FAILED) { PFATAL("Unable to mmap '%s'", path); }

        /* See what happens. We rely on save_if_interesting() to catch major
           errors and save the test case. */

        (void)write_to_testcase(afl, (void **)&mem, st.st_size, 1);

        fault = fuzz_run_target(afl, &afl->fsrv, afl->fsrv.exec_tmout);

        if (afl->stop_soon) { goto close_sync; }

        afl->syncing_party = sd_ent->d_name;
        afl->queued_imported +=
            save_if_interesting(afl, mem, st.st_size, fault);
        afl->syncing_party = 0;

        munmap(mem, st.st_size);

      }

      close(fd);

    }

    ck_write(id_fd, &next_min_accept, sizeof(u32), qd_synced_path);

  close_sync:
    close(id_fd);
    if (n > 0)
      for (m = 0; m < n; m++)
        free(namelist[m]);
    free(namelist);

  }

  closedir(sd);

  // If we are a secondary and no main was found to sync then become the main
  if (unlikely(synced == 0) && likely(entries) &&
      likely(afl->is_secondary_node)) {

    // there is a small race condition here that another secondary runs at the
    // same time. If so, the first temporary main node running again will demote
    // themselves so this is not an issue

    //    u8 path2[PATH_MAX];
    afl->is_main_node = 1;
    sprintf(path, "%s/is_main_node", afl->out_dir);
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd >= 0) { close(fd); }

  }

  if (afl->foreign_sync_cnt) read_foreign_testcases(afl, 0);

  afl->last_sync_time = get_cur_time();
  afl->last_sync_cycle = afl->queue_cycle;

}

/* Trim all new test cases to save cycles when doing deterministic checks. The
   trimmer uses power-of-two increments somewhere between 1/16 and 1/1024 of
   file size, to keep the stage short and sweet. */

u8 trim_case(afl_state_t *afl, struct queue_entry *q, u8 *in_buf) {

  u32 orig_len = q->len;

  /* Custom mutator trimmer */
  if (afl->custom_mutators_count) {

    u8   trimmed_case = 0;
    bool custom_trimmed = false;

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_trim) {

        trimmed_case = trim_case_custom(afl, q, in_buf, el);
        custom_trimmed = true;

      }

    });

    if (orig_len != q->len || custom_trimmed) {

      queue_testcase_retake(afl, q, orig_len);

    }

    if (custom_trimmed) return trimmed_case;

  }

  u8  needs_write = 0, fault = 0;
  u32 trim_exec = 0;
  u32 remove_len;
  u32 len_p2;

  u8 val_bufs[2][STRINGIFY_VAL_SIZE_MAX];

  /* Although the trimmer will be less useful when variable behavior is
     detected, it will still work to some extent, so we don't check for
     this. */

  if (unlikely(q->len < 5)) { return 0; }

  afl->stage_name = afl->stage_name_buf;
  afl->bytes_trim_in += q->len;

  /* Select initial chunk len, starting with large steps. */

  len_p2 = next_pow2(q->len);

  remove_len = MAX(len_p2 / TRIM_START_STEPS, (u32)TRIM_MIN_BYTES);

  /* Continue until the number of steps gets too high or the stepover
     gets too small. */

  while (remove_len >= MAX(len_p2 / TRIM_END_STEPS, (u32)TRIM_MIN_BYTES)) {

    u32 remove_pos = remove_len;

    sprintf(afl->stage_name_buf, "trim %s/%s",
            u_stringify_int(val_bufs[0], remove_len),
            u_stringify_int(val_bufs[1], remove_len));

    afl->stage_cur = 0;
    afl->stage_max = q->len / remove_len;

    while (remove_pos < q->len) {

      u32 trim_avail = MIN(remove_len, q->len - remove_pos);
      u64 cksum;

      write_with_gap(afl, in_buf, q->len, remove_pos, trim_avail);

      fault = fuzz_run_target(afl, &afl->fsrv, afl->fsrv.exec_tmout);

      if (afl->stop_soon || fault == FSRV_RUN_ERROR) { goto abort_trimming; }

      /* Note that we don't keep track of crashes or hangs here; maybe TODO?
       */

      ++afl->trim_execs;
      classify_counts(&afl->fsrv);
      cksum = hash64(afl->fsrv.trace_bits, afl->fsrv.map_size, HASH_CONST);

      /* If the deletion had no impact on the trace, make it permanent. This
         isn't perfect for variable-path inputs, but we're just making a
         best-effort pass, so it's not a big deal if we end up with false
         negatives every now and then. */

      if (cksum == q->exec_cksum) {

        u32 move_tail = q->len - remove_pos - trim_avail;

        q->len -= trim_avail;
        len_p2 = next_pow2(q->len);

        memmove(in_buf + remove_pos, in_buf + remove_pos + trim_avail,
                move_tail);

        /* Let's save a clean trace, which will be needed by
           update_bitmap_score once we're done with the trimming stuff. */

        if (!needs_write) {

          needs_write = 1;
          memcpy(afl->clean_trace, afl->fsrv.trace_bits, afl->fsrv.map_size);

        }

      } else {

        remove_pos += remove_len;

      }

      /* Since this can be slow, update the screen every now and then. */

      if (!(trim_exec++ % afl->stats_update_freq)) { show_stats(afl); }
      ++afl->stage_cur;

    }

    remove_len >>= 1;

  }

  /* If we have made changes to in_buf, we also need to update the on-disk
     version of the test case. */

  if (needs_write) {

    s32 fd;

    if (unlikely(afl->no_unlink)) {

      fd = open(q->fname, O_WRONLY | O_CREAT | O_TRUNC, DEFAULT_PERMISSION);

      if (fd < 0) { PFATAL("Unable to create '%s'", q->fname); }

      u32 written = 0;
      while (written < q->len) {

        ssize_t result = write(fd, in_buf, q->len - written);
        if (result > 0) written += result;

      }

    } else {

      unlink(q->fname);                                    /* ignore errors */
      fd = open(q->fname, O_WRONLY | O_CREAT | O_EXCL, DEFAULT_PERMISSION);

      if (fd < 0) { PFATAL("Unable to create '%s'", q->fname); }

      ck_write(fd, in_buf, q->len, q->fname);

    }

    close(fd);

    queue_testcase_retake_mem(afl, q, in_buf, q->len, orig_len);

    memcpy(afl->fsrv.trace_bits, afl->clean_trace, afl->fsrv.map_size);
    update_bitmap_score(afl, q);

  }

abort_trimming:

  afl->bytes_trim_out += q->len;
  return fault;

}

/* Write a modified test case, run program, process results. Handle
   error conditions, returning 1 if it's time to bail out. This is
   a helper function for fuzz_one(). */

u8 __attribute__((hot))
common_fuzz_stuff(afl_state_t *afl, u8 *out_buf, u32 len) {

  u8 fault;

  if (unlikely(len = write_to_testcase(afl, (void **)&out_buf, len, 0)) == 0) {

    return 0;

  }

  fault = fuzz_run_target(afl, &afl->fsrv, afl->fsrv.exec_tmout);

  if (afl->stop_soon) { return 1; }

  if (fault == FSRV_RUN_TMOUT) {

    if (afl->subseq_tmouts++ > TMOUT_LIMIT) {

      ++afl->cur_skipped_items;
      return 1;

    }

  } else {

    afl->subseq_tmouts = 0;

  }

  /* Users can hit us with SIGUSR1 to request the current input
     to be abandoned. */

  if (afl->skip_requested) {

    afl->skip_requested = 0;
    ++afl->cur_skipped_items;
    return 1;

  }

  /* This handles FAULT_ERROR for us: */

  afl->queued_discovered += save_if_interesting(afl, out_buf, len, fault);

  if (!(afl->stage_cur % afl->stats_update_freq) ||
      afl->stage_cur + 1 == afl->stage_max) {

    show_stats(afl);

  }

  return 0;

}

