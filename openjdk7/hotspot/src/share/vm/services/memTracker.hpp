/*
 * Copyright (c) 2012, 2013, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#ifndef SHARE_VM_SERVICES_MEM_TRACKER_HPP
#define SHARE_VM_SERVICES_MEM_TRACKER_HPP

#include "memory/allocation.hpp"
#include "runtime/globals.hpp"
#include "runtime/mutex.hpp"
#include "runtime/os.hpp"
#include "runtime/thread.hpp"
#include "services/memPtr.hpp"
#include "services/memRecorder.hpp"
#include "services/memSnapshot.hpp"
#include "services/memTrackWorker.hpp"

#ifdef SOLARIS
#include "thread_solaris.inline.hpp"
#endif

extern bool NMT_track_callsite;

#ifndef MAX_UNSIGNED_LONG
#define MAX_UNSIGNED_LONG    (unsigned long)(-1)
#endif

#ifdef ASSERT
  #define DEBUG_CALLER_PC  (NMT_track_callsite ? os::get_caller_pc(2) : 0)
#else
  #define DEBUG_CALLER_PC  0
#endif

// The thread closure walks threads to collect per-thread
// memory recorders at NMT sync point
class SyncThreadRecorderClosure : public ThreadClosure {
 private:
  int _thread_count;

 public:
  SyncThreadRecorderClosure() {
    _thread_count =0;
  }

  void do_thread(Thread* thread);
  int  get_thread_count() const {
    return _thread_count;
  }
};

class BaselineOutputer;
class MemSnapshot;
class MemTrackWorker;
class Thread;
/*
 * MemTracker is the 'gate' class to native memory tracking runtime.
 */
class MemTracker : AllStatic {
  friend class GenerationData;
  friend class MemTrackWorker;
  friend class MemSnapshot;
  friend class SyncThreadRecorderClosure;

  // NMT state
  enum NMTStates {
    NMT_uninited,                        // not yet initialized
    NMT_bootstrapping_single_thread,     // bootstrapping, VM is in single thread mode
    NMT_bootstrapping_multi_thread,      // bootstrapping, VM is about to enter multi-thread mode
    NMT_started,                         // NMT fully started
    NMT_shutdown_pending,                // shutdown pending
    NMT_final_shutdown,                  // in final phase of shutdown
    NMT_shutdown                         // shutdown
  };

 public:
  // native memory tracking level
  enum NMTLevel {
    NMT_off,              // native memory tracking is off
    NMT_summary,          // don't track callsite
    NMT_detail            // track callsite also
  };

   enum ShutdownReason {
     NMT_shutdown_none,     // no shutdown requested
     NMT_shutdown_user,     // user requested shutdown
     NMT_normal,            // normal shutdown, process exit
     NMT_out_of_memory,     // shutdown due to out of memory
     NMT_initialization,    // shutdown due to initialization failure
     NMT_use_malloc_only,   // can not combine NMT with UseMallocOnly flag
     NMT_error_reporting,   // shutdown by vmError::report_and_die()
     NMT_out_of_generation, // running out of generation queue
     NMT_sequence_overflow  // overflow the sequence number
   };

 public:
  // initialize NMT tracking level from command line options, called
   // from VM command line parsing code
  static void init_tracking_options(const char* option_line);

  // if NMT is enabled to record memory activities
  static inline bool is_on() {
    return (_tracking_level >= NMT_summary &&
      _state >= NMT_bootstrapping_single_thread);
  }

  static inline enum NMTLevel tracking_level() {
    return _tracking_level;
  }

  // user readable reason for shutting down NMT
  static const char* reason() {
    switch(_reason) {
      case NMT_shutdown_none:
        return "Native memory tracking is not enabled";
      case NMT_shutdown_user:
        return "Native memory tracking has been shutdown by user";
      case NMT_normal:
        return "Native memory tracking has been shutdown due to process exiting";
      case NMT_out_of_memory:
        return "Native memory tracking has been shutdown due to out of native memory";
      case NMT_initialization:
        return "Native memory tracking failed to initialize";
      case NMT_error_reporting:
        return "Native memory tracking has been shutdown due to error reporting";
      case NMT_out_of_generation:
        return "Native memory tracking has been shutdown due to running out of generation buffer";
      case NMT_sequence_overflow:
        return "Native memory tracking has been shutdown due to overflow the sequence number";
      case NMT_use_malloc_only:
        return "Native memory tracking is not supported when UseMallocOnly is on";
      default:
        ShouldNotReachHere();
        return NULL;
    }
  }

  // test if we can walk native stack
  static bool can_walk_stack() {
  // native stack is not walkable during bootstrapping on sparc
#if defined(SPARC)
    return (_state == NMT_started);
#else
    return (_state >= NMT_bootstrapping_single_thread && _state  <= NMT_started);
#endif
  }

  // if native memory tracking tracks callsite
  static inline bool track_callsite() { return _tracking_level == NMT_detail; }

  // NMT automatically shuts itself down under extreme situation by default.
  // When the value is set to false,  NMT will try its best to stay alive,
  // even it has to slow down VM.
  static inline void set_autoShutdown(bool value) {
    AutoShutdownNMT = value;
    if (AutoShutdownNMT && _slowdown_calling_thread) {
      _slowdown_calling_thread = false;
    }
  }

  // shutdown native memory tracking capability. Native memory tracking
  // can be shutdown by VM when it encounters low memory scenarios.
  // Memory tracker should gracefully shutdown itself, and preserve the
  // latest memory statistics for post morten diagnosis.
  static void shutdown(ShutdownReason reason);

  // if there is shutdown requested
  static inline bool shutdown_in_progress() {
    return (_state >= NMT_shutdown_pending);
  }

  // bootstrap native memory tracking, so it can start to collect raw data
  // before worker thread can start

  // the first phase of bootstrapping, when VM still in single-threaded mode
  static void bootstrap_single_thread();
  // the second phase of bootstrapping, VM is about or already in multi-threaded mode
  static void bootstrap_multi_thread();


  // start() has to be called when VM still in single thread mode, but after
  // command line option parsing is done.
  static void start();

  // record a 'malloc' call
  static inline void record_malloc(address addr, size_t size, MEMFLAGS flags,
                            address pc = 0, Thread* thread = NULL) {
    if (is_on() && NMT_CAN_TRACK(flags)) {
      assert(size > 0, "Sanity check");
      create_memory_record(addr, (flags|MemPointerRecord::malloc_tag()), size, pc, thread);
    }
  }
  // record a 'free' call
  static inline void record_free(address addr, MEMFLAGS flags, Thread* thread = NULL) {
    if (is_on() && NMT_CAN_TRACK(flags)) {
      create_memory_record(addr, MemPointerRecord::free_tag(), 0, 0, thread);
    }
  }
  // record a 'realloc' call
  static inline void record_realloc(address old_addr, address new_addr, size_t size,
       MEMFLAGS flags, address pc = 0, Thread* thread = NULL) {
    if (is_on() && NMT_CAN_TRACK(flags)) {
      assert(size > 0, "Sanity check");
      record_free(old_addr, flags, thread);
      record_malloc(new_addr, size, flags, pc, thread);
    }
  }

  // record arena memory size
  static inline void record_arena_size(address addr, size_t size) {
    // we add a positive offset to arena address, so we can have arena memory record
    // sorted after arena record
    if (is_on() && !UseMallocOnly) {
      assert(addr != NULL, "Sanity check");
      create_memory_record((addr + sizeof(void*)), MemPointerRecord::arena_size_tag(), size,
        DEBUG_CALLER_PC, NULL);
    }
  }

  // record a virtual memory 'reserve' call
  static inline void record_virtual_memory_reserve(address addr, size_t size,
                            address pc = 0, Thread* thread = NULL) {
    if (is_on()) {
      assert(size > 0, "Sanity check");
      create_memory_record(addr, MemPointerRecord::virtual_memory_reserve_tag(),
                           size, pc, thread);
    }
  }

  static inline void record_thread_stack(address addr, size_t size, Thread* thr,
                           address pc = 0) {
    if (is_on()) {
      assert(size > 0 && thr != NULL, "Sanity check");
      create_memory_record(addr, MemPointerRecord::virtual_memory_reserve_tag() | mtThreadStack,
                          size, pc, thr);
      create_memory_record(addr, MemPointerRecord::virtual_memory_commit_tag() | mtThreadStack,
                          size, pc, thr);
    }
  }

  static inline void release_thread_stack(address addr, size_t size, Thread* thr) {
    if (is_on()) {
      assert(size > 0 && thr != NULL, "Sanity check");
      assert(!thr->is_Java_thread(), "too early");
      create_memory_record(addr, MemPointerRecord::virtual_memory_uncommit_tag() | mtThreadStack,
                          size, DEBUG_CALLER_PC, thr);
      create_memory_record(addr, MemPointerRecord::virtual_memory_release_tag() | mtThreadStack,
                          size, DEBUG_CALLER_PC, thr);
    }
  }

  // record a virtual memory 'commit' call
  static inline void record_virtual_memory_commit(address addr, size_t size,
                            address pc, Thread* thread = NULL) {
    if (is_on()) {
      assert(size > 0, "Sanity check");
      create_memory_record(addr, MemPointerRecord::virtual_memory_commit_tag(),
                           size, pc, thread);
    }
  }

  // record a virtual memory 'uncommit' call
  static inline void record_virtual_memory_uncommit(address addr, size_t size,
                            Thread* thread = NULL) {
    if (is_on()) {
      assert(size > 0, "Sanity check");
      create_memory_record(addr, MemPointerRecord::virtual_memory_uncommit_tag(),
                           size, DEBUG_CALLER_PC, thread);
    }
  }

  // record a virtual memory 'release' call
  static inline void record_virtual_memory_release(address addr, size_t size,
                            Thread* thread = NULL) {
    if (is_on()) {
      assert(size > 0, "Sanity check");
      create_memory_record(addr, MemPointerRecord::virtual_memory_release_tag(),
                           size, DEBUG_CALLER_PC, thread);
    }
  }

  // record memory type on virtual memory base address
  static inline void record_virtual_memory_type(address base, MEMFLAGS flags,
                            Thread* thread = NULL) {
    if (is_on()) {
      assert(base > 0, "wrong base address");
      assert((flags & (~mt_masks)) == 0, "memory type only");
      create_memory_record(base, (flags | MemPointerRecord::virtual_memory_type_tag()),
                           0, DEBUG_CALLER_PC, thread);
    }
  }


  // create memory baseline of current memory snapshot
  static bool baseline();
  // is there a memory baseline
  static bool has_baseline() {
    return _baseline.baselined();
  }

  // print memory usage from current snapshot
  static bool print_memory_usage(BaselineOutputer& out, size_t unit,
           bool summary_only = true);
  // compare memory usage between current snapshot and baseline
  static bool compare_memory_usage(BaselineOutputer& out, size_t unit,
           bool summary_only = true);

  // the version for whitebox testing support, it ensures that all memory
  // activities before this method call, are reflected in the snapshot
  // database.
  static bool wbtest_wait_for_data_merge();

  // sync is called within global safepoint to synchronize nmt data
  static void sync();

  // called when a thread is about to exit
  static void thread_exiting(JavaThread* thread);

  // retrieve global snapshot
  static MemSnapshot* get_snapshot() {
    if (shutdown_in_progress()) {
      return NULL;
    }
    return _snapshot;
  }

  // print tracker stats
  NOT_PRODUCT(static void print_tracker_stats(outputStream* st);)
  NOT_PRODUCT(static void walk_stack(int toSkip, char* buf, int len);)

 private:
  // start native memory tracking worker thread
  static bool start_worker(MemSnapshot* snapshot);

  // called by worker thread to complete shutdown process
  static void final_shutdown();

 protected:
  // retrieve per-thread recorder of the specified thread.
  // if the recorder is full, it will be enqueued to overflow
  // queue, a new recorder is acquired from recorder pool or a
  // new instance is created.
  // when thread == NULL, it means global recorder
  static MemRecorder* get_thread_recorder(JavaThread* thread);

  // per-thread recorder pool
  static void release_thread_recorder(MemRecorder* rec);
  static void delete_all_pooled_recorders();

  // pending recorder queue. Recorders are queued to pending queue
  // when they are overflowed or collected at nmt sync point.
  static void enqueue_pending_recorder(MemRecorder* rec);
  static MemRecorder* get_pending_recorders();
  static void delete_all_pending_recorders();

 private:
  // retrieve a pooled memory record or create new one if there is not
  // one available
  static MemRecorder* get_new_or_pooled_instance();
  static void create_memory_record(address addr, MEMFLAGS type,
                   size_t size, address pc, Thread* thread);
  static void create_record_in_recorder(address addr, MEMFLAGS type,
                   size_t size, address pc, JavaThread* thread);

  static void set_current_processing_generation(unsigned long generation) {
    _worker_thread_idle = false;
    _processing_generation = generation;
  }

  static void report_worker_idle() {
    _worker_thread_idle = true;
  }

 private:
  // global memory snapshot
  static MemSnapshot*     _snapshot;

  // a memory baseline of snapshot
  static MemBaseline      _baseline;

  // query lock
  static Mutex*           _query_lock;

  // a thread can start to allocate memory before it is attached
  // to VM 'Thread', those memory activities are recorded here.
  // ThreadCritical is required to guard this global recorder.
  static MemRecorder* volatile _global_recorder;

  // main thread id
  debug_only(static intx   _main_thread_tid;)

  // pending recorders to be merged
  static MemRecorder* volatile     _merge_pending_queue;

  NOT_PRODUCT(static volatile jint   _pending_recorder_count;)

  // pooled memory recorders
  static MemRecorder* volatile     _pooled_recorders;

  // memory recorder pool management, uses following
  // counter to determine if a released memory recorder
  // should be pooled

  // latest thread count
  static int               _thread_count;
  // pooled recorder count
  static volatile jint     _pooled_recorder_count;


  // worker thread to merge pending recorders into snapshot
  static MemTrackWorker*  _worker_thread;

  // how many safepoints we skipped without entering sync point
  static int              _sync_point_skip_count;

  // if the tracker is properly intialized
  static bool             _is_tracker_ready;
  // tracking level (off, summary and detail)
  static enum NMTLevel    _tracking_level;

  // current nmt state
  static volatile enum NMTStates   _state;
  // the reason for shutting down nmt
  static enum ShutdownReason       _reason;
  // the generation that NMT is processing
  static volatile unsigned long    _processing_generation;
  // although NMT is still procesing current generation, but
  // there is not more recorder to process, set idle state
  static volatile bool             _worker_thread_idle;

  // if NMT should slow down calling thread to allow
  // worker thread to catch up
  static volatile bool             _slowdown_calling_thread;
};

#endif // SHARE_VM_SERVICES_MEM_TRACKER_HPP
