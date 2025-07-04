/*
   american fuzzy lop++ - test case minimizer
   ------------------------------------------

   Originally written by Michal Zalewski

   Forkserver design by Jann Horn <jannhorn@googlemail.com>

   Now maintained by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eissfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com> and
                        Dominik Maier <mail@dmnk.co>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019-2024 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     https://www.apache.org/licenses/LICENSE-2.0

   A simple test case minimizer that takes an input file and tries to remove
   as much data as possible while keeping the binary in a crashing state
   *or* producing consistent instrumentation output (the mode is auto-selected
   based on the initially observed behavior).

 */

#define AFL_MAIN

#include "config.h"
#include "types.h"
#include "debug.h"
#include "alloc-inl.h"
#include "hash.h"
#include "forkserver.h"
#include "sharedmem.h"
#include "common.h"
#include "afl-fuzz.h"
#include "list.h"

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>

#include <sys/wait.h>
#include <sys/time.h>
#ifndef USEMMAP
  #include <sys/shm.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#ifdef USE_PYTHON
  #include <Python.h>
#endif

extern void destroy_custom_mutators(afl_state_t *);
void        list_init(list_t *list) {

  if (list) {

    list->element_prealloc_count = 0;
    memset(list->element_prealloc_buf, 0, sizeof(list->element_prealloc_buf));

  }

}

void                   setup_custom_mutators(afl_state_t *);
struct custom_mutator *load_custom_mutator(afl_state_t *, const char *);
#ifdef USE_PYTHON
struct custom_mutator *load_custom_mutator_py(afl_state_t *, char *);
#endif

static afl_state_t *afl;               /* State for custom mutators         */
static u8          *mask_bitmap;       /* Mask for trace bits (-B)          */

static u8 *in_file,                    /* Minimizer input test case         */
    *out_file, *output_file;           /* Minimizer output file             */

static u8 *in_data;                    /* Input data for trimming           */

static u32 in_len,                     /* Input data length                 */
    missed_hangs,                      /* Misses due to hangs               */
    missed_crashes,                    /* Misses due to crashes             */
    missed_paths,                      /* Misses due to exec path diffs     */
    map_size = MAP_SIZE;

static u64 orig_cksum;                 /* Original checksum                 */

static u8 crash_mode,                  /* Crash-centric mode?               */
    hang_mode,                         /* Minimize as long as it hangs      */
    exit_crash,                        /* Treat non-zero exit as crash?     */
    edges_only,                        /* Ignore hit counts?                */
    exact_mode,                        /* Require path match for crashes?   */
    remove_out_file,                   /* remove out_file on exit?          */
    remove_shm = 1,                    /* remove shmem on exit?             */
    debug;                             /* debug mode                        */

static u32 del_len_limit = 1;          /* Minimum block deletion length     */

static volatile u8 stop_soon;          /* Ctrl-C pressed?                   */

static afl_forkserver_t *fsrv;
static sharedmem_t       shm;
static sharedmem_t      *shm_fuzz;

/*
 * forkserver section
 */

/* Classify tuple counts. This is a slow & naive version, but good enough here.
 */

static const u8 count_class_lookup[256] = {

    [0] = 0,
    [1] = 1,
    [2] = 2,
    [3] = 4,
    [4 ... 7] = 8,
    [8 ... 15] = 16,
    [16 ... 31] = 32,
    [32 ... 127] = 64,
    [128 ... 255] = 128

};

static void kill_child(int signal) {

  (void)signal;
  if (fsrv->child_pid > 0) {

    kill(fsrv->child_pid, fsrv->child_kill_signal);
    fsrv->child_pid = -1;

  }

}

static sharedmem_t *deinit_shmem(afl_forkserver_t *fsrv,
                                 sharedmem_t      *shm_fuzz) {

  afl_shm_deinit(shm_fuzz);
  fsrv->support_shmem_fuzz = 0;
  fsrv->shmem_fuzz_len = NULL;
  fsrv->shmem_fuzz = NULL;
  ck_free(shm_fuzz);
  return NULL;

}

/* dummy functions */
u32 write_to_testcase(afl_state_t *afl, void **mem, u32 a, u32 b) {

  (void)afl;
  (void)mem;
  return a + b;

}

void show_stats(afl_state_t *afl) {

  (void)afl;

}

void update_bitmap_score(afl_state_t *afl, struct queue_entry *q,
                         bool add_to_queue) {

  (void)afl;
  (void)q;
  (void)add_to_queue;

}

fsrv_run_result_t fuzz_run_target(afl_state_t *afl, afl_forkserver_t *fsrv,
                                  u32 i) {

  (void)afl;
  (void)fsrv;
  (void)i;
  return 0;

}

#ifndef USE_PYTHON
struct custom_mutator *load_custom_mutator_py(afl_state_t *afl, char *module) {

  (void)afl;
  (void)module;
  FATAL("Python support not available in this build");
  return NULL;

}

#endif

/* Apply mask to classified bitmap (if set). */

static void apply_mask(u32 *mem, u32 *mask) {

  u32 i = (map_size >> 2);

  if (!mask) { return; }

  while (i--) {

    *mem &= ~*mask;
    mem++;
    mask++;

  }

}

void classify_counts(afl_forkserver_t *fsrv) {

  u8 *mem = fsrv->trace_bits;
  u32 i = map_size;

  if (edges_only) {

    while (i--) {

      if (*mem) { *mem = 1; }
      mem++;

    }

  } else {

    while (i--) {

      *mem = count_class_lookup[*mem];
      mem++;

    }

  }

}

/* See if any bytes are set in the bitmap. */

static inline u8 anything_set(afl_forkserver_t *fsrv) {

  u32 *ptr = (u32 *)fsrv->trace_bits;
  u32  i = (map_size >> 2);

  while (i--) {

    if (*(ptr++)) { return 1; }

  }

  return 0;

}

static void at_exit_handler(void) {

  if (remove_shm) {

    if (shm.map) afl_shm_deinit(&shm);
    if (fsrv->use_shmem_fuzz) deinit_shmem(fsrv, shm_fuzz);

  }

  afl_fsrv_killall();
  if (remove_out_file) unlink(out_file);

  if (afl) {

    destroy_custom_mutators(afl);
    ck_free(afl);

  }

}

/* Read initial file. */

static void read_initial_file(void) {

  struct stat st;
  s32         fd = open(in_file, O_RDONLY);

  if (fd < 0) { PFATAL("Unable to open '%s'", in_file); }

  if (fstat(fd, &st) || !st.st_size) { FATAL("Zero-sized input file."); }

  if (st.st_size >= TMIN_MAX_FILE) {

    FATAL("Input file is too large (%ld MB max)", TMIN_MAX_FILE / 1024 / 1024);

  }

  in_len = st.st_size;
  in_data = ck_alloc_nozero(in_len);

  ck_read(fd, in_data, in_len, in_file);

  close(fd);

  OKF("Read %u byte%s from '%s'.", in_len, in_len == 1 ? "" : "s", in_file);

}

/* Write output file. */

static s32 write_to_file(u8 *path, u8 *mem, u32 len) {

  s32 ret;

  unlink(path);                                            /* Ignore errors */

  ret = open(path, O_RDWR | O_CREAT | O_EXCL, DEFAULT_PERMISSION);

  if (ret < 0) { PFATAL("Unable to create '%s'", path); }

  ck_write(ret, mem, len, path);

  lseek(ret, 0, SEEK_SET);

  return ret;

}

/* Helper function to handle custom mutators for testcase writing */
static void pre_afl_fsrv_write_to_testcase(afl_forkserver_t *fsrv, u8 *mem,
                                           u32 len) {

  static u8 buf[MAX_FILE];
  u32       sent = 0;

  if (afl && afl->custom_mutators_count) {

    ssize_t new_size = len;
    u8     *new_mem = mem;
    u8     *new_buf = NULL;

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_post_process) {

        new_size =
            el->afl_custom_post_process(el->data, new_mem, new_size, &new_buf);

        if (!new_buf || new_size <= 0) {

          return;

        } else {

          new_mem = new_buf;
          len = new_size;

        }

      }

    });

    if (new_mem != mem && new_mem != NULL) {

      mem = buf;
      memcpy(mem, new_mem, new_size);

    }

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_fuzz_send) {

        el->afl_custom_fuzz_send(el->data, mem, len);
        sent = 1;

      }

    });

  }

  if (!sent) { afl_fsrv_write_to_testcase(fsrv, mem, len); }

}

/* Execute target application. Returns 0 if the changes are a dud, or
   1 if they should be kept. */

static u8 tmin_run_target(afl_forkserver_t *fsrv, u8 *mem, u32 len,
                          u8 first_run) {

  pre_afl_fsrv_write_to_testcase(fsrv, mem, len);
  fsrv_run_result_t ret =
      afl_fsrv_run_target(fsrv, fsrv->exec_tmout, &stop_soon);

  if (ret == FSRV_RUN_ERROR) { FATAL("Couldn't run child"); }

  if (stop_soon) {

    SAYF(cRST cLRD "\n+++ Minimization aborted by user +++\n" cRST);
    close(write_to_file(output_file, in_data, in_len));
    exit(1);

  }

  /* Always discard inputs that time out, unless we are in hang mode */

  if (hang_mode) {

    switch (ret) {

      case FSRV_RUN_TMOUT:
        return 1;
      case FSRV_RUN_CRASH:
        missed_crashes++;
        return 0;
      default:
        missed_hangs++;
        return 0;

    }

  }

  classify_counts(fsrv);
  apply_mask((u32 *)fsrv->trace_bits, (u32 *)mask_bitmap);

  if (ret == FSRV_RUN_TMOUT) {

    missed_hangs++;
    return 0;

  }

  /* Handle crashing inputs depending on current mode. */

  if (ret == FSRV_RUN_CRASH) {

    if (first_run) { crash_mode = 1; }

    if (crash_mode) {

      if (!exact_mode) { return 1; }

    } else {

      missed_crashes++;
      return 0;

    }

  } else {

    /* Handle non-crashing inputs appropriately. */

    if (crash_mode) {

      missed_paths++;
      return 0;

    }

  }

  if (ret == FSRV_RUN_NOINST) { FATAL("Binary not instrumented?"); }

  u64 cksum = hash64(fsrv->trace_bits, fsrv->map_size, HASH_CONST);

  if (first_run) { orig_cksum = cksum; }

  if (orig_cksum == cksum) { return 1; }

  missed_paths++;
  return 0;

}

/* Actually minimize! */

static void minimize(afl_forkserver_t *fsrv) {

  static u32 alpha_map[256];
  u8        *tmp_buf = ck_alloc_nozero(in_len);
  u32        orig_len = in_len, stage_o_len;
  u32        del_len, set_len, del_pos, set_pos, i, alpha_size, cur_pass = 0;
  u32 syms_removed, alpha_del0 = 0, alpha_del1, alpha_del2, alpha_d_total = 0;
  u8  changed_any, prev_del;

#ifdef USE_PYTHON
  // Try to load python module
  char *py_module = getenv("AFL_PYTHON_MODULE");
  if (py_module) {

    // We cannot use Python custom mutators in tmin
    if (debug) WARNF("Python custom mutator support not available in afl-tmin");

  }

#endif

  // Custom mutator trimming
  if (afl && afl->custom_mutators_count) {

    LIST_FOREACH(&afl->custom_mutator_list, struct custom_mutator, {

      if (el->afl_custom_init_trim && el->afl_custom_trim &&
          el->afl_custom_post_trim) {

        ACTF("Performing custom trim with %s...", el->name);

        // Initialize the trimmer
        s32 initial_steps = el->afl_custom_init_trim(el->data, in_data, in_len);

        if (initial_steps <= 0) {

          WARNF("Custom trimmer %s returned %d, skipping", el->name,
                initial_steps);
          continue;

        }

        ACTF("Custom trimmer initialized, %d steps planned", initial_steps);

        u32 trim_rounds = 0;
        u32 trimmed_successfully = 0;

        // Trim loop
        s32 cur_step = 0;
        while (cur_step < initial_steps) {

          u8    *trimmed_buf = NULL;
          size_t trimmed_size;

          u8 *retbuf = NULL;
          trimmed_size = el->afl_custom_trim(el->data, &retbuf);

          // If trimmed_size equals or exceeds original size, skip
          if (trimmed_size >= in_len) {

            SAYF("[Custom trim] Round %u: no improvements over %u bytes.\n",
                 trim_rounds, in_len);
            el->afl_custom_post_trim(el->data, 0);
            cur_step++;
            trim_rounds++;
            continue;

          }

          trimmed_buf = retbuf;

          // Test if the trimmed case still works
          if (!tmin_run_target(fsrv, trimmed_buf, trimmed_size, 0)) {

            SAYF(
                "[Custom trim] But the testcase no longer reproduces - "
                "skipping this reduction.\n");
            el->afl_custom_post_trim(el->data, 0);
            if (trimmed_buf != in_data) { ck_free(trimmed_buf); }

          } else {

            // Accept the reduction
            u8 *old_in_data = in_data;
            in_data = trimmed_buf;
            in_len = trimmed_size;

            trimmed_successfully = 1;
            el->afl_custom_post_trim(el->data, 1);

            SAYF("[Custom trim] Successful reduction to %u bytes\n", in_len);

            if (old_in_data != in_data && old_in_data != trimmed_buf) {

              ck_free(old_in_data);

            }

          }

          cur_step++;
          trim_rounds++;

        }

        ACTF("Custom trimming with %s complete after %u rounds, reduced: %s",
             el->name, trim_rounds, trimmed_successfully ? "yes" : "no");

        if (trimmed_successfully) {

          if (tmp_buf) { ck_free(tmp_buf); }
          return;  // Skip standard minimization if successful

        }

      }

    });

  }

  // Skip built-in minimization if in_len is too small
  if (in_len <= 1) {

    if (tmp_buf) { ck_free(tmp_buf); }
    return;

  }

  /***********************
   * BLOCK NORMALIZATION *
   ***********************/

  set_len = next_pow2(in_len / TMIN_SET_STEPS);
  set_pos = 0;

  if (set_len < TMIN_SET_MIN_SIZE) { set_len = TMIN_SET_MIN_SIZE; }

  ACTF(cBRI "Stage #0: " cRST "One-time block normalization...");

  while (set_pos < in_len) {

    u32 use_len = MIN(set_len, in_len - set_pos);

    for (i = 0; i < use_len; i++) {

      if (in_data[set_pos + i] != '0') { break; }

    }

    if (i != use_len) {

      memcpy(tmp_buf, in_data, in_len);
      memset(tmp_buf + set_pos, '0', use_len);

      u8 res;
      res = tmin_run_target(fsrv, tmp_buf, in_len, 0);

      if (res) {

        memset(in_data + set_pos, '0', use_len);
        /*        changed_any = 1; value is not used */
        alpha_del0 += use_len;

      }

    }

    set_pos += set_len;

  }

  alpha_d_total += alpha_del0;

  OKF("Block normalization complete, %u byte%s replaced.", alpha_del0,
      alpha_del0 == 1 ? "" : "s");

next_pass:

  ACTF(cYEL "--- " cBRI "Pass #%u " cYEL "---", ++cur_pass);
  changed_any = 0;

  /******************
   * BLOCK DELETION *
   ******************/

  del_len = next_pow2(in_len / TRIM_START_STEPS);
  stage_o_len = in_len;

  ACTF(cBRI "Stage #1: " cRST "Removing blocks of data...");

next_del_blksize:

  if (!del_len) { del_len = 1; }
  del_pos = 0;
  prev_del = 1;

  SAYF(cGRA "    Block length = %u, remaining size = %u\n" cRST, del_len,
       in_len);

  while (del_pos < in_len) {

    u8  res;
    s32 tail_len;

    tail_len = in_len - del_pos - del_len;
    if (tail_len < 0) { tail_len = 0; }

    /* If we have processed at least one full block (initially, prev_del == 1),
       and we did so without deleting the previous one, and we aren't at the
       very end of the buffer (tail_len > 0), and the current block is the same
       as the previous one... skip this step as a no-op. */

    if (!prev_del && tail_len &&
        !memcmp(in_data + del_pos - del_len, in_data + del_pos, del_len)) {

      del_pos += del_len;
      continue;

    }

    prev_del = 0;

    /* Head */
    memcpy(tmp_buf, in_data, del_pos);

    /* Tail */
    memcpy(tmp_buf + del_pos, in_data + del_pos + del_len, tail_len);

    res = tmin_run_target(fsrv, tmp_buf, del_pos + tail_len, 0);

    if (res) {

      memcpy(in_data, tmp_buf, del_pos + tail_len);
      prev_del = 1;
      in_len = del_pos + tail_len;

      changed_any = 1;

    } else {

      del_pos += del_len;

    }

  }

  if (del_len > del_len_limit && in_len >= 1) {

    del_len /= 2;
    goto next_del_blksize;

  }

  OKF("Block removal complete, %u bytes deleted.", stage_o_len - in_len);

  if (!in_len && changed_any) {

    WARNF(cLRD
          "Down to zero bytes - check the command line and mem limit!" cRST);

  }

  if (cur_pass > 1 && !changed_any) { goto finalize_all; }

  /*************************
   * ALPHABET MINIMIZATION *
   *************************/

  alpha_size = 0;
  alpha_del1 = 0;
  syms_removed = 0;

  memset(alpha_map, 0, sizeof(alpha_map));

  for (i = 0; i < in_len; i++) {

    if (!alpha_map[in_data[i]]) { alpha_size++; }
    alpha_map[in_data[i]]++;

  }

  ACTF(cBRI "Stage #2: " cRST "Minimizing symbols (%u code point%s)...",
       alpha_size, alpha_size == 1 ? "" : "s");

  for (i = 0; i < 256; i++) {

    u32 r;
    u8  res;

    if (i == '0' || !alpha_map[i]) { continue; }

    memcpy(tmp_buf, in_data, in_len);

    for (r = 0; r < in_len; r++) {

      if (tmp_buf[r] == i) { tmp_buf[r] = '0'; }

    }

    res = tmin_run_target(fsrv, tmp_buf, in_len, 0);

    if (res) {

      memcpy(in_data, tmp_buf, in_len);
      syms_removed++;
      alpha_del1 += alpha_map[i];
      changed_any = 1;

    }

  }

  alpha_d_total += alpha_del1;

  OKF("Symbol minimization finished, %u symbol%s (%u byte%s) replaced.",
      syms_removed, syms_removed == 1 ? "" : "s", alpha_del1,
      alpha_del1 == 1 ? "" : "s");

  /**************************
   * CHARACTER MINIMIZATION *
   **************************/

  alpha_del2 = 0;

  ACTF(cBRI "Stage #3: " cRST "Character minimization...");

  memcpy(tmp_buf, in_data, in_len);

  for (i = 0; i < in_len; i++) {

    u8 res, orig = tmp_buf[i];

    if (orig == '0') { continue; }
    tmp_buf[i] = '0';

    res = tmin_run_target(fsrv, tmp_buf, in_len, 0);

    if (res) {

      in_data[i] = '0';
      alpha_del2++;
      changed_any = 1;

    } else {

      tmp_buf[i] = orig;

    }

  }

  alpha_d_total += alpha_del2;

  OKF("Character minimization done, %u byte%s replaced.", alpha_del2,
      alpha_del2 == 1 ? "" : "s");

  if (changed_any) { goto next_pass; }

finalize_all:

  if (tmp_buf) { ck_free(tmp_buf); }

  if (hang_mode) {

    SAYF("\n" cGRA "     File size reduced by : " cRST
         "%0.02f%% (to %u byte%s)\n" cGRA "    Characters simplified : " cRST
         "%0.02f%%\n" cGRA "     Number of execs done : " cRST "%llu\n" cGRA
         "          Fruitless execs : " cRST "termination=%u crash=%u\n\n",
         100 - ((double)in_len) * 100 / orig_len, in_len,
         in_len == 1 ? "" : "s",
         ((double)(alpha_d_total)) * 100 / (in_len ? in_len : 1),
         fsrv->total_execs, missed_paths, missed_crashes);
    return;

  }

  SAYF("\n" cGRA "     File size reduced by : " cRST
       "%0.02f%% (to %u byte%s)\n" cGRA "    Characters simplified : " cRST
       "%0.02f%%\n" cGRA "     Number of execs done : " cRST "%llu\n" cGRA
       "          Fruitless execs : " cRST "path=%u crash=%u hang=%s%u\n\n",
       100 - ((double)in_len) * 100 / orig_len, in_len, in_len == 1 ? "" : "s",
       ((double)(alpha_d_total)) * 100 / (in_len ? in_len : 1),
       fsrv->total_execs, missed_paths, missed_crashes,
       missed_hangs ? cLRD : "", missed_hangs);

  if (fsrv->total_execs > 50 && missed_hangs * 10 > fsrv->total_execs &&
      !hang_mode) {

    WARNF(cLRD "Frequent timeouts - results may be skewed." cRST);

  }

}

/* Handle Ctrl-C and the like. */

static void handle_stop_sig(int sig) {

  (void)sig;
  stop_soon = 1;
  afl_fsrv_killall();

}

/* Do basic preparations - persistent fds, filenames, etc. */

static void set_up_environment(afl_forkserver_t *fsrv, char **argv) {

  u8 *x;

  fsrv->dev_null_fd = open("/dev/null", O_RDWR);
  if (fsrv->dev_null_fd < 0) { PFATAL("Unable to open /dev/null"); }

  if (!out_file) {

    u8 *use_dir = ".";

    if (access(use_dir, R_OK | W_OK | X_OK)) {

      use_dir = get_afl_env("TMPDIR");
      if (!use_dir) { use_dir = "/tmp"; }

    }

    out_file = alloc_printf("%s/.afl-tmin-temp-%u", use_dir, (u32)getpid());
    remove_out_file = 1;

  }

  unlink(out_file);

  fsrv->out_file = out_file;
  fsrv->out_fd = open(out_file, O_RDWR | O_CREAT | O_EXCL, DEFAULT_PERMISSION);

  if (fsrv->out_fd < 0) { PFATAL("Unable to create '%s'", out_file); }

  /* Set sane defaults... */

  x = get_afl_env("MSAN_OPTIONS");

  if (x) {

    if (!strstr(x, "exit_code=" STRINGIFY(MSAN_ERROR))) {

      FATAL("Custom MSAN_OPTIONS set without exit_code=" STRINGIFY(
          MSAN_ERROR) " - please fix!");

    }

  }

  set_sanitizer_defaults();
  afl_fsrv_setup_preload(fsrv, argv[0]);

}

/* Setup signal handlers, duh. */

void setup_signal_handlers(void) {

  struct sigaction sa;

  sa.sa_handler = NULL;
#ifdef SA_RESTART
  sa.sa_flags = SA_RESTART;
#else
  sa.sa_flags = 0;
#endif
  sa.sa_sigaction = NULL;

  sigemptyset(&sa.sa_mask);

  /* Various ways of saying "stop". */

  sa.sa_handler = handle_stop_sig;
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

}

/* Display usage hints. */

static void usage(u8 *argv0) {

  SAYF(
      "\n%s [ options ] -- /path/to/target_app [ ... ]\n\n"

      "Required parameters:\n"

      "  -i file       - input test case to be shrunk by the tool\n"
      "  -o file       - final output location for the minimized data\n\n"

      "Execution control settings:\n"

      "  -f file       - input file read by the tested program (stdin)\n"
      "  -t msec       - timeout for each run (%u ms)\n"
      "  -m megs       - memory limit for child process (%u MB)\n"
#if defined(__linux__) && defined(__aarch64__)
      "  -A            - use binary-only instrumentation (ARM CoreSight mode)\n"
#endif
      "  -O            - use binary-only instrumentation (FRIDA mode)\n"
#if defined(__linux__)
      "  -Q            - use binary-only instrumentation (QEMU mode)\n"
      "  -U            - use unicorn-based instrumentation (Unicorn mode)\n"
      "  -W            - use qemu-based instrumentation with Wine (Wine "
      "mode)\n"
      "                  (Not necessary, here for consistency with other afl-* "
      "tools)\n"
      "  -X            - use Nyx mode\n"
#endif
      "\n"

      "Minimization settings:\n"

      "  -e            - solve for edge coverage only, ignore hit counts\n"
      "  -l bytes      - set minimum block deletion length to speed up minimization\n"
      "  -x            - treat non-zero exit codes as crashes\n"
      "  -H            - minimize a hang (hang mode)\n\n"

      "For additional tips, please consult %s/README.md.\n\n"

      "Environment variables used:\n"
      "AFL_CRASH_EXITCODE: optional child exit code to be interpreted as crash\n"
      "AFL_FORKSRV_INIT_TMOUT: time spent waiting for forkserver during startup (in ms)\n"
      "AFL_KILL_SIGNAL: Signal ID delivered to child processes on timeout, etc.\n"
      "                 (default: SIGKILL)\n"
      "AFL_FORK_SERVER_KILL_SIGNAL: Kill signal for the fork server on termination\n"
      "                             (default: SIGTERM). If unset and AFL_KILL_SIGNAL is\n"
      "                             set, that value will be used.\n"
      "AFL_MAP_SIZE: the shared memory size for that target. must be >= the size\n"
      "              the target was compiled for\n"
      "AFL_PRELOAD:  LD_PRELOAD / DYLD_INSERT_LIBRARIES settings for target\n"
      "AFL_TMIN_EXACT: require execution paths to match for crashing inputs\n"
      "AFL_NO_FORKSRV: run target via execve instead of using the forkserver\n"
      "ASAN_OPTIONS: custom settings for ASAN\n"
      "              (must contain abort_on_error=1 and symbolize=0)\n"
      "MSAN_OPTIONS: custom settings for MSAN\n"
      "              (must contain exitcode="STRINGIFY(MSAN_ERROR)" and symbolize=0)\n"
      "TMPDIR: directory to use for temporary input files\n",
      argv0, EXEC_TIMEOUT, MEM_LIMIT, doc_path);

  exit(1);

}

/* Main entry point */

int main(int argc, char **argv_orig, char **envp) {

  s32 opt;
  u8  mem_limit_given = 0, timeout_given = 0, unicorn_mode = 0, use_wine = 0,
     del_limit_given = 0;
  char **use_argv;

  char **argv = argv_cpy_dup(argc, argv_orig);

  afl_forkserver_t fsrv_var = {0};
  if (getenv("AFL_DEBUG")) { debug = 1; }
  fsrv = &fsrv_var;
  afl_fsrv_init(fsrv);
  map_size = get_map_size();
  fsrv->map_size = map_size;

  doc_path = access(DOC_PATH, F_OK) ? "docs" : DOC_PATH;

  SAYF(cCYA "afl-tmin" VERSION cRST " by Michal Zalewski\n");

  while ((opt = getopt(argc, argv, "+i:o:f:m:t:l:B:xeAOQUWXYHh")) > 0) {

    switch (opt) {

      case 'i':

        if (in_file) { FATAL("Multiple -i options not supported"); }
        in_file = optarg;
        break;

      case 'o':

        if (output_file) { FATAL("Multiple -o options not supported"); }
        output_file = optarg;
        break;

      case 'f':

        if (out_file) { FATAL("Multiple -f options not supported"); }
        fsrv->use_stdin = 0;
        out_file = ck_strdup(optarg);
        break;

      case 'e':

        if (edges_only) { FATAL("Multiple -e options not supported"); }
        if (hang_mode) {

          FATAL("Edges only and hang mode are mutually exclusive.");

        }

        edges_only = 1;
        break;

      case 'x':

        if (exit_crash) { FATAL("Multiple -x options not supported"); }
        exit_crash = 1;
        break;

      case 'm': {

        u8 suffix = 'M';

        if (mem_limit_given) { FATAL("Multiple -m options not supported"); }
        mem_limit_given = 1;

        if (!optarg) { FATAL("Wrong usage of -m"); }

        if (!strcmp(optarg, "none")) {

          fsrv->mem_limit = 0;
          break;

        }

        if (sscanf(optarg, "%llu%c", &fsrv->mem_limit, &suffix) < 1 ||
            optarg[0] == '-') {

          FATAL("Bad syntax used for -m");

        }

        switch (suffix) {

          case 'T':
            fsrv->mem_limit *= 1024 * 1024;
            break;
          case 'G':
            fsrv->mem_limit *= 1024;
            break;
          case 'k':
            fsrv->mem_limit /= 1024;
            break;
          case 'M':
            break;

          default:
            FATAL("Unsupported suffix or bad syntax for -m");

        }

        if (fsrv->mem_limit < 5) { FATAL("Dangerously low value of -m"); }

        if (sizeof(rlim_t) == 4 && fsrv->mem_limit > 2000) {

          FATAL("Value of -m out of range on 32-bit systems");

        }

      }

      break;

      case 't':

        if (timeout_given) { FATAL("Multiple -t options not supported"); }
        timeout_given = 1;

        if (!optarg) { FATAL("Wrong usage of -t"); }

        fsrv->exec_tmout = atoi(optarg);

        if (fsrv->exec_tmout < 10 || optarg[0] == '-') {

          FATAL("Dangerously low value of -t");

        }

        break;

      case 'A':                                           /* CoreSight mode */

#if !defined(__aarch64__) || !defined(__linux__)
        FATAL("-A option is not supported on this platform");
#endif

        if (fsrv->cs_mode) { FATAL("Multiple -A options not supported"); }

        fsrv->cs_mode = 1;
        break;

      case 'O':                                               /* FRIDA mode */

        if (fsrv->frida_mode) { FATAL("Multiple -O options not supported"); }

        fsrv->frida_mode = 1;
        setenv("AFL_FRIDA_INST_SEED", "1", 1);

        break;

      case 'Q':

        if (fsrv->qemu_mode) { FATAL("Multiple -Q options not supported"); }
        if (!mem_limit_given) { fsrv->mem_limit = MEM_LIMIT_QEMU; }

        fsrv->qemu_mode = 1;
        break;

      case 'U':

        if (unicorn_mode) { FATAL("Multiple -Q options not supported"); }
        if (!mem_limit_given) { fsrv->mem_limit = MEM_LIMIT_UNICORN; }

        unicorn_mode = 1;
        break;

      case 'W':                                           /* Wine+QEMU mode */

        if (use_wine) { FATAL("Multiple -W options not supported"); }
        fsrv->qemu_mode = 1;
        use_wine = 1;

        if (!mem_limit_given) { fsrv->mem_limit = 0; }

        break;

      case 'Y':  // fallthrough
#ifdef __linux__
      case 'X':                                                 /* NYX mode */

        if (fsrv->nyx_mode) { FATAL("Multiple -X options not supported"); }

        fsrv->nyx_mode = 1;
        fsrv->nyx_parent = true;
        fsrv->nyx_standalone = true;

        break;
#else
      case 'X':
        FATAL("Nyx mode is only available on linux...");
        break;
#endif

      case 'H':                                                /* Hang Mode */

        /* Minimizes a testcase to the minimum that still times out */

        if (hang_mode) { FATAL("Multiple -H options not supported"); }
        if (edges_only) {

          FATAL("Edges only and hang mode are mutually exclusive.");

        }

        hang_mode = 1;
        break;

      case 'B':                                              /* load bitmap */

        /* This is a secret undocumented option! It is speculated to be useful
           if you have a baseline "boring" input file and another "interesting"
           file you want to minimize.

           You can dump a binary bitmap for the boring file using
           afl-showmap -b, and then load it into afl-tmin via -B. The minimizer
           will then minimize to preserve only the edges that are unique to
           the interesting input file, but ignoring everything from the
           original map.

           The option may be extended and made more official if it proves
           to be useful. */

        if (mask_bitmap) { FATAL("Multiple -B options not supported"); }
        mask_bitmap = ck_alloc(map_size);
        read_bitmap(optarg, mask_bitmap, map_size);
        break;

      case 'l':
        if (del_limit_given) { FATAL("Multiple -l options not supported"); }
        del_limit_given = 1;

        if (!optarg) { FATAL("Wrong usage of -l"); }

        if (optarg[0] == '-') { FATAL("Dangerously low value of -l"); }

        del_len_limit = atoi(optarg);

        if (del_len_limit < 1 || del_len_limit > TMIN_MAX_FILE) {

          FATAL("Value of -l out of range between 1 and TMIN_MAX_FILE");

        }

        break;

      case 'h':
        usage(argv[0]);
        return -1;
        break;

      default:
        usage(argv[0]);

    }

  }

  if (optind == argc || !in_file || !output_file) { usage(argv[0]); }

  check_environment_vars(envp);

  if (getenv("AFL_NO_FORKSRV")) {             /* if set, use the fauxserver */
    fsrv->use_fauxsrv = true;

  }

  setenv("AFL_NO_AUTODICT", "1", 1);

  /* initialize cmplog_mode */
  shm.cmplog_mode = 0;

  atexit(at_exit_handler);
  setup_signal_handlers();

  set_up_environment(fsrv, argv);

#ifdef __linux__
  if (!fsrv->nyx_mode) {

    fsrv->target_path = find_binary(argv[optind]);

  } else {

    fsrv->target_path = ck_strdup(argv[optind]);

  }

#else
  fsrv->target_path = find_binary(argv[optind]);
#endif

  fsrv->trace_bits = afl_shm_init(&shm, map_size, 0);
  detect_file_args(argv + optind, out_file, &fsrv->use_stdin);
  signal(SIGALRM, kill_child);

  if (fsrv->qemu_mode) {

    if (use_wine) {

      use_argv = get_wine_argv(argv[0], &fsrv->target_path, argc - optind,
                               argv + optind);

    } else {

      use_argv = get_qemu_argv(argv[0], &fsrv->target_path, argc - optind,
                               argv + optind);

    }

  } else if (fsrv->cs_mode) {

    use_argv =
        get_cs_argv(argv[0], &fsrv->target_path, argc - optind, argv + optind);

#ifdef __linux__

  } else if (fsrv->nyx_mode) {

    fsrv->nyx_id = 0;

    u8 *libnyx_binary = find_afl_binary(argv[0], "libnyx.so");
    fsrv->nyx_handlers = afl_load_libnyx_plugin(libnyx_binary);
    if (fsrv->nyx_handlers == NULL) {

      FATAL("failed to initialize libnyx.so...");

    }

    fsrv->nyx_use_tmp_workdir = true;
    fsrv->nyx_bind_cpu_id = 0;

    use_argv = argv + optind;
#endif

  } else {

    use_argv = argv + optind;

  }

  exact_mode = !!get_afl_env("AFL_TMIN_EXACT");

  if (hang_mode && exact_mode) {

    SAYF("AFL_TMIN_EXACT won't work for loops in hang mode, ignoring.");
    exact_mode = 0;

  }

  SAYF("\n");

  if (getenv("AFL_FORKSRV_INIT_TMOUT")) {

    s32 forksrv_init_tmout = atoi(getenv("AFL_FORKSRV_INIT_TMOUT"));
    if (forksrv_init_tmout < 1) {

      FATAL("Bad value specified for AFL_FORKSRV_INIT_TMOUT");

    }

    fsrv->init_tmout = (u32)forksrv_init_tmout;

  }

  configure_afl_kill_signals(
      fsrv, NULL, NULL, (fsrv->qemu_mode || unicorn_mode) ? SIGKILL : SIGTERM);

  if (getenv("AFL_CRASH_EXITCODE")) {

    long exitcode = strtol(getenv("AFL_CRASH_EXITCODE"), NULL, 10);
    if ((!exitcode && (errno == EINVAL || errno == ERANGE)) ||
        exitcode < -127 || exitcode > 128) {

      FATAL("Invalid crash exitcode, expected -127 to 128, but got %s",
            getenv("AFL_CRASH_EXITCODE"));

    }

    fsrv->uses_crash_exitcode = true;
    // WEXITSTATUS is 8 bit unsigned
    fsrv->crash_exitcode = (u8)exitcode;

  }

  shm_fuzz = ck_alloc(sizeof(sharedmem_t));

  /* initialize cmplog_mode */
  shm_fuzz->cmplog_mode = 0;

  size_t shm_fuzz_map_size = SHM_FUZZ_MAP_SIZE_DEFAULT;
  u8    *map = afl_shm_init(shm_fuzz, shm_fuzz_map_size, 1);
  shm_fuzz->shmemfuzz_mode = 1;
  if (!map) { FATAL("BUG: Zero return from afl_shm_init."); }

  u8 *shm_fuzz_map_size_str = alloc_printf("%zu", shm_fuzz_map_size);
  setenv(SHM_FUZZ_MAP_SIZE_ENV_VAR, shm_fuzz_map_size_str, 1);
  ck_free(shm_fuzz_map_size_str);

#ifdef USEMMAP
  setenv(SHM_FUZZ_ENV_VAR, shm_fuzz->g_shm_file_path, 1);
#else
  u8 *shm_str = alloc_printf("%d", shm_fuzz->shm_id);
  setenv(SHM_FUZZ_ENV_VAR, shm_str, 1);
  ck_free(shm_str);
#endif
  fsrv->support_shmem_fuzz = 1;
  fsrv->shmem_fuzz_len = (u32 *)map;
  fsrv->shmem_fuzz = map + sizeof(u32);

  read_initial_file();

  // Initialize AFL state for custom mutators
  afl = calloc(1, sizeof(afl_state_t));
  if (afl) {

    list_init(&afl->custom_mutator_list);
    afl->custom_mutators_count = 0;

    afl->fsrv.dev_urandom_fd = open("/dev/urandom", O_RDONLY);
    if (afl->fsrv.dev_urandom_fd < 0) { PFATAL("Unable to open /dev/urandom"); }

    afl->afl_env.afl_custom_mutator_library =
        getenv("AFL_CUSTOM_MUTATOR_LIBRARY");
    afl->afl_env.afl_python_module = getenv("AFL_PYTHON_MODULE");

    afl->shm = shm;
    afl->out_dir = dirname(in_file);

    memcpy(&afl->fsrv, fsrv, sizeof(afl_forkserver_t));

    setup_custom_mutators(afl);

  }

#ifdef __linux__
  if (!fsrv->nyx_mode) { (void)check_binary_signatures(fsrv->target_path); }
#else
  (void)check_binary_signatures(fsrv->target_path);
#endif

  if (!fsrv->qemu_mode && !unicorn_mode) {

    fsrv->map_size = 4194304;  // dummy temporary value
    u32 new_map_size =
        afl_fsrv_get_mapsize(fsrv, use_argv, &stop_soon,
                             (get_afl_env("AFL_DEBUG_CHILD") ||
                              get_afl_env("AFL_DEBUG_CHILD_OUTPUT"))
                                 ? 1
                                 : 0);

    if (new_map_size) {

      if (map_size < new_map_size ||
          (new_map_size > map_size && new_map_size - map_size > MAP_SIZE)) {

        if (!be_quiet)
          ACTF("Acquired new map size for target: %u bytes\n", new_map_size);

        afl_shm_deinit(&shm);
        afl_fsrv_kill(fsrv);
        fsrv->map_size = new_map_size;
        fsrv->trace_bits = afl_shm_init(&shm, new_map_size, 0);
        afl_fsrv_start(fsrv, use_argv, &stop_soon,
                       (get_afl_env("AFL_DEBUG_CHILD") ||
                        get_afl_env("AFL_DEBUG_CHILD_OUTPUT"))
                           ? 1
                           : 0);

      }

      map_size = new_map_size;

    }

    fsrv->map_size = map_size;

  } else {

    afl_fsrv_start(fsrv, use_argv, &stop_soon,
                   (get_afl_env("AFL_DEBUG_CHILD") ||
                    get_afl_env("AFL_DEBUG_CHILD_OUTPUT"))
                       ? 1
                       : 0);

  }

  if (fsrv->support_shmem_fuzz && !fsrv->use_shmem_fuzz)
    shm_fuzz = deinit_shmem(fsrv, shm_fuzz);

  ACTF("Performing dry run (mem limit = %llu MB, timeout = %u ms%s)...",
       fsrv->mem_limit, fsrv->exec_tmout, edges_only ? ", edges only" : "");

  tmin_run_target(fsrv, in_data, in_len, 1);

  if (hang_mode && !fsrv->last_run_timed_out) {

    FATAL(
        "Target binary did not time out but hang minimization mode "
        "(-H) was set (-t %u).",
        fsrv->exec_tmout);

  }

  if (fsrv->last_run_timed_out && !hang_mode) {

    FATAL(
        "Target binary times out (adjusting -t may help). Use -H to minimize a "
        "hang.");

  }

  if (hang_mode) {

    OKF("Program hangs as expected, minimizing in " cCYA "hang" cRST " mode.");

  } else if (!crash_mode) {

    OKF("Program terminates normally, minimizing in " cCYA "instrumented" cRST
        " mode.");

    if (!anything_set(fsrv)) { FATAL("No instrumentation detected."); }

  } else {

    OKF("Program exits with a signal, minimizing in " cMGN "%scrash" cRST
        " mode.",
        exact_mode ? "EXACT " : "");

  }

  minimize(fsrv);

  ACTF("Writing output to '%s'...", output_file);

  unlink(out_file);
  if (out_file) { ck_free(out_file); }
  out_file = NULL;

  close(write_to_file(output_file, in_data, in_len));

  OKF("We're done here. Have a nice day!\n");

  remove_shm = 0;
  afl_shm_deinit(&shm);
  if (fsrv->use_shmem_fuzz) shm_fuzz = deinit_shmem(fsrv, shm_fuzz);
  afl_fsrv_deinit(fsrv);
  if (fsrv->target_path) { ck_free(fsrv->target_path); }
  if (mask_bitmap) { ck_free(mask_bitmap); }
  if (in_data) { ck_free(in_data); }

  argv_cpy_free(argv);

  exit(0);

}

