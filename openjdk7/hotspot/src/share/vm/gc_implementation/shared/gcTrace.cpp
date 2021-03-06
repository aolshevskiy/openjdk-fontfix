/*
 * Copyright (c) 2012, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "gc_implementation/shared/gcHeapSummary.hpp"
#include "gc_implementation/shared/gcTimer.hpp"
#include "gc_implementation/shared/gcTrace.hpp"
#include "gc_implementation/shared/copyFailedInfo.hpp"
#include "memory/heapInspection.hpp"
#include "memory/iterator.hpp"
#include "memory/referenceProcessorStats.hpp"
#include "utilities/globalDefinitions.hpp"

#ifndef SERIALGC
#include "gc_implementation/g1/evacuationInfo.hpp"
#endif

#define assert_unset_gc_id() assert(_shared_gc_info.id() == SharedGCInfo::UNSET_GCID, "GC already started?")
#define assert_set_gc_id() assert(_shared_gc_info.id() != SharedGCInfo::UNSET_GCID, "GC not started?")

static jlong GCTracer_next_gc_id = 0;
static GCId create_new_gc_id() {
  return GCTracer_next_gc_id++;
}

void GCTracer::report_gc_start_impl(GCCause::Cause cause, jlong timestamp) {
  assert_unset_gc_id();

  GCId gc_id = create_new_gc_id();
  _shared_gc_info.set_id(gc_id);
  _shared_gc_info.set_cause(cause);
  _shared_gc_info.set_start_timestamp(timestamp);
}

void GCTracer::report_gc_start(GCCause::Cause cause, jlong timestamp) {
  assert_unset_gc_id();

  report_gc_start_impl(cause, timestamp);
}

bool GCTracer::has_reported_gc_start() const {
  return _shared_gc_info.id() != SharedGCInfo::UNSET_GCID;
}

void GCTracer::report_gc_end_impl(jlong timestamp, TimePartitions* time_partitions) {
  assert_set_gc_id();

  _shared_gc_info.set_sum_of_pauses(time_partitions->sum_of_pauses());
  _shared_gc_info.set_longest_pause(time_partitions->longest_pause());
  _shared_gc_info.set_end_timestamp(timestamp);

  send_phase_events(time_partitions);
  send_garbage_collection_event();
}

void GCTracer::report_gc_end(jlong timestamp, TimePartitions* time_partitions) {
  assert_set_gc_id();

  report_gc_end_impl(timestamp, time_partitions);

  _shared_gc_info.set_id(SharedGCInfo::UNSET_GCID);
}

void GCTracer::report_gc_reference_stats(const ReferenceProcessorStats& rps) const {
  assert_set_gc_id();

  send_reference_stats_event(REF_SOFT, rps.soft_count());
  send_reference_stats_event(REF_WEAK, rps.weak_count());
  send_reference_stats_event(REF_FINAL, rps.final_count());
  send_reference_stats_event(REF_PHANTOM, rps.phantom_count());
}

class ObjectCountEventSenderClosure : public KlassInfoClosure {
  GCTracer* _gc_tracer;
 public:
  ObjectCountEventSenderClosure(GCTracer* gc_tracer) : _gc_tracer(gc_tracer) {}
 private:
  void do_cinfo(KlassInfoEntry* entry) {
    if (is_visible_klass(entry->klass())) {
      _gc_tracer->send_object_count_after_gc_event(entry->klass(), entry->count(),
                                                   entry->words() * BytesPerWord);
      }
  }

  // Do not expose internal implementation specific classes
  bool is_visible_klass(klassOop k) {
    return k->klass_part()->oop_is_instance() ||
           (k->klass_part()->oop_is_array() && k != Universe::systemObjArrayKlassObj());
  }
};

void GCTracer::report_object_count_after_gc(BoolObjectClosure *is_alive_cl) {
  if (should_send_object_count_after_gc_event()) {
    ResourceMark rm;

    KlassInfoTable cit(HeapInspection::start_of_perm_gen());
    if (!cit.allocation_failed()) {
      ObjectCountEventSenderClosure event_sender(this);
      HeapInspection::instance_inspection(&cit, &event_sender, false, is_alive_cl);
    }
  }
}

void GCTracer::report_gc_heap_summary(GCWhen::Type when, const GCHeapSummary& heap_summary, const PermGenSummary& perm_gen_summary) const {
  assert_set_gc_id();

  send_gc_heap_summary_event(when, heap_summary);
  send_perm_gen_summary_event(when, perm_gen_summary);
}

void YoungGCTracer::report_gc_end_impl(jlong timestamp, TimePartitions* time_partitions) {
  assert_set_gc_id();
  assert(_tenuring_threshold != UNSET_TENURING_THRESHOLD, "Tenuring threshold has not been reported");

  GCTracer::report_gc_end_impl(timestamp, time_partitions);
  send_young_gc_event();

  _tenuring_threshold = UNSET_TENURING_THRESHOLD;
}

void YoungGCTracer::report_promotion_failed(const PromotionFailedInfo& pf_info) {
  assert_set_gc_id();

  send_promotion_failed_event(pf_info);
}

void YoungGCTracer::report_tenuring_threshold(const uint tenuring_threshold) {
  _tenuring_threshold = tenuring_threshold;
}

void OldGCTracer::report_gc_end_impl(jlong timestamp, TimePartitions* time_partitions) {
  assert_set_gc_id();

  GCTracer::report_gc_end_impl(timestamp, time_partitions);
  send_old_gc_event();
}

void ParallelOldTracer::report_gc_end_impl(jlong timestamp, TimePartitions* time_partitions) {
  assert_set_gc_id();

  OldGCTracer::report_gc_end_impl(timestamp, time_partitions);
  send_parallel_old_event();
}

void ParallelOldTracer::report_dense_prefix(void* dense_prefix) {
  assert_set_gc_id();

  _parallel_old_gc_info.report_dense_prefix(dense_prefix);
}

void OldGCTracer::report_concurrent_mode_failure() {
  assert_set_gc_id();

  send_concurrent_mode_failure_event();
}

#ifndef SERIALGC
void G1NewTracer::report_yc_type(G1YCType type) {
  assert_set_gc_id();

  _g1_young_gc_info.set_type(type);
}

void G1NewTracer::report_gc_end_impl(jlong timestamp, TimePartitions* time_partitions) {
  assert_set_gc_id();

  YoungGCTracer::report_gc_end_impl(timestamp, time_partitions);
  send_g1_young_gc_event();
}

void G1NewTracer::report_evacuation_info(EvacuationInfo* info) {
  assert_set_gc_id();

  send_evacuation_info_event(info);
}

void G1NewTracer::report_evacuation_failed(EvacuationFailedInfo& ef_info) {
  assert_set_gc_id();

  send_evacuation_failed_event(ef_info);
  ef_info.reset();
}
#endif
