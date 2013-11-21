// Copyright 2013 Cloudera Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.


#ifndef IMPALA_RUNTIME_MEM_TRACKER_H
#define IMPALA_RUNTIME_MEM_TRACKER_H

#include <stdint.h>
#include <map>
#include <vector>
#include <boost/shared_ptr.hpp>
#include <boost/weak_ptr.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/unordered_map.hpp>

#include "common/logging.h"
#include "common/atomic.h"
#include "util/debug-util.h"
#include "util/internal-queue.h"
#include "util/metrics.h"
#include "util/runtime-profile.h"
#include "util/spinlock.h"

#include "gen-cpp/Types_types.h" // for TUniqueId

namespace impala {

class MemTracker;

// A MemTracker tracks memory consumption; it contains an optional limit
// and can be arranged into a tree structure such that the consumption tracked
// by a MemTracker is also tracked by its ancestors.
//
// By default, memory consumption is tracked via calls to Consume()/Release(), either to
// the tracker itself or to one of its descendents. Alternatively, a consumption metric
// can specified, and then the metric's value is used as the consumption rather than the
// tally maintained by Consume() and Release(). A tcmalloc metric is used to track process
// memory consumption, since the process memory usage may be higher than the computed
// total memory (tcmalloc does not release deallocated memory immediately).
//
// GcFunctions can be attached to a MemTracker in order to free up memory if the limit is
// reached. If LimitExceeded() is called and the limit is exceeded, it will first call the
// GcFunctions to try to free memory and recheck the limit. For example, the process
// tracker has a GcFunction that releases any unused memory still held by tcmalloc, so
// this will be called before the process limit is reported as exceeded. GcFunctions are
// called in the order they are added, so expensive functions should be added last.
//
// This class is thread-safe.
class MemTracker {
 public:
  // byte_limit < 0 means no limit
  // 'label' is the label used in the usage string (LogUsage())
  MemTracker(int64_t byte_limit = -1, const std::string& label = std::string(),
             MemTracker* parent = NULL);

  // C'tor for tracker for which consumption counter is created as part of a profile.
  // The counter is created with name COUNTER_NAME.
  MemTracker(RuntimeProfile* profile, int64_t byte_limit,
             const std::string& label = std::string(), MemTracker* parent = NULL);

  // C'tor for tracker that uses consumption_metric as the consumption value.
  // Consume()/Release() can still be called. This is used for the process tracker.
  MemTracker(Metrics::PrimitiveMetric<uint64_t>* consumption_metric,
             int64_t byte_limit = -1, const std::string& label = std::string());

  ~MemTracker();

  // Removes this tracker from parent_->child_trackers_.
  void UnregisterFromParent();

  // Returns a MemTracker object for query 'id'.  Calling this with the same id will
  // return the same MemTracker object.  An example of how this is used is to pass it
  // the same query id for all fragments of that query running on this machine.  This
  // way, we have per-query limits rather than per-fragment.
  // The first time this is called for an id, a new MemTracker object is created with
  // 'parent' as the parent tracker.
  // byte_limit and parent must be the same for all GetMemTracker() calls with the
  // same id.
  static boost::shared_ptr<MemTracker> GetQueryMemTracker(
      const TUniqueId& id, int64_t byte_limit, MemTracker* parent);

  // Increases consumption of this tracker and its ancestors by 'bytes'.
  void Consume(int64_t bytes) {
    if (consumption_metric_ != NULL) {
      DCHECK(parent_ == NULL);
      consumption_->Set(consumption_metric_->value());
      return;
    }
    if (bytes == 0) return;
    if (UNLIKELY(enable_logging_)) LogUpdate(true, bytes);
    for (std::vector<MemTracker*>::iterator tracker = all_trackers_.begin();
         tracker != all_trackers_.end(); ++tracker) {
      (*tracker)->consumption_->Update(bytes);
      DCHECK_GE((*tracker)->consumption_->current_value(), 0);
    }
  }

  // Increases consumption of this tracker and its ancestors by 'bytes' only if
  // they can all consume 'bytes'. If this brings any of them over, none of them
  // are updated.
  // Returns true if the try succeeded.
  bool TryConsume(int64_t bytes) {
    if (consumption_metric_ != NULL) consumption_->Set(consumption_metric_->value());
    if (bytes == 0) return true;
    if (UNLIKELY(enable_logging_)) LogUpdate(true, bytes);
    int i = 0;
    for (; i < all_trackers_.size(); ++i) {
      if (all_trackers_[i]->limit_ < 0) {
        all_trackers_[i]->consumption_->Update(bytes);
      } else {
        if (!all_trackers_[i]->consumption_->TryUpdate(bytes, all_trackers_[i]->limit_)) {
          // One of the trackers failed, attempt to GC memory. If that succeeds,
          // TryUpdate() again. Bail if either fails.
          if (all_trackers_[i]->GcMemory(all_trackers_[i]->limit_ - bytes) ||
              !all_trackers_[i]->consumption_->TryUpdate(
                  bytes, all_trackers_[i]->limit_)) {
            break;
          }
        }
      }
    }
    // Everyone succeeded, return.
    if (i == all_trackers_.size()) return true;

    // Someone failed, roll back the ones that succeeded.
    // TODO: this doesn't roll it back completely since the max values for
    // the updated trackers aren't decremented. The max values are only used
    // for error reporting so this is probably okay. Rolling those back is
    // pretty hard; we'd need something like 2PC.
    for (int j = 0; j < i; ++j) {
      all_trackers_[j]->consumption_->Update(-bytes);
    }
    return false;
  }

  // Decreases consumption of this tracker and its ancestors by 'bytes'.
  void Release(int64_t bytes) {
    if (consumption_metric_ != NULL) {
      DCHECK(parent_ == NULL);
      consumption_->Set(consumption_metric_->value());
      return;
    }
    if (bytes == 0) return;
    if (UNLIKELY(enable_logging_)) LogUpdate(false, bytes);
    for (std::vector<MemTracker*>::iterator tracker = all_trackers_.begin();
         tracker != all_trackers_.end(); ++tracker) {
      (*tracker)->consumption_->Update(-bytes);
      DCHECK_GE((*tracker)->consumption_->current_value(), 0);
    }
  }

  // Returns true if a valid limit of this tracker or one of its ancestors is
  // exceeded.
  bool AnyLimitExceeded() {
    for (std::vector<MemTracker*>::iterator tracker = limit_trackers_.begin();
         tracker != limit_trackers_.end(); ++tracker) {
      if ((*tracker)->LimitExceeded()) return true;
    }
    return false;
  }

  // If this tracker has a limit, checks the limit and attempts to free up some memory if
  // the limit is exceeded by calling any added GC functions. Returns true if the limit is
  // exceeded after calling the GC functions. Returns false if there is no limit.
  bool LimitExceeded() {
    if (UNLIKELY(CheckLimitExceeded())) {
      if (bytes_over_limit_metric_ != NULL) {
        bytes_over_limit_metric_->Update(consumption() - limit_);
      }
      return GcMemory(limit_);
    }
    return false;
  }

  int64_t limit() const { return limit_; }
  bool has_limit() const { return limit_ >= 0; }
  const std::string& label() const { return label_; }

  // Returns the memory consumed in bytes.
  int64_t consumption() const { return consumption_->current_value(); }

  // Note that if consumption_ is based on consumption_metric_, this will the max value
  // we've recorded in consumption(), not necessarily the highest value
  // consumption_metric_ has ever reached.
  int64_t peak_consumption() const { return consumption_->value(); }

  MemTracker* parent() const { return parent_; }

  // Signature for function that can be called to free some memory after limit is reached.
  typedef boost::function<void ()> GcFunction;

  // Add a function 'f' to be called if the limit is reached.
  // 'f' does not need to be thread-safe as long as it is added to only one MemTracker.
  // Note that 'f' must be valid for the lifetime of this MemTracker.
  void AddGcFunction(GcFunction f) { gc_functions_.push_back(f); }

  // Register this MemTracker's metrics. Each key will be of the form
  // "<prefix>.<metric name>".
  void RegisterMetrics(Metrics* metrics, const std::string& prefix);

  // Logs the usage of this tracker and all of its children (recursively).
  std::string LogUsage(const std::string& prefix = "") const;

  void EnableLogging(bool enable, bool log_stack) {
    enable_logging_ = enable;
    log_stack_ = log_stack;
  }

  static const std::string COUNTER_NAME;

 private:
  bool CheckLimitExceeded() const { return limit_ >= 0 && limit_ < consumption(); }

  // If consumption is higher than max_consumption, attempts to free memory by calling any
  // added GC functions.  Returns true if max_consumption is still exceeded. Takes
  // gc_lock. Updates metrics if initialized.
  bool GcMemory(int64_t max_consumption);

  // Lock to protect GcMemory(). This prevents many GCs from occurring at once.
  SpinLock gc_lock_;

  // All MemTracker objects that are in use and lock protecting it.
  // For memory management, this map contains only weak ptrs.  MemTrackers that are
  // handed out via GetQueryMemTracker() are shared ptrs.  When all the shared ptrs are
  // no longer referenced, the MemTracker d'tor will be called at which point the
  // weak ptr will be removed from the map.
  typedef boost::unordered_map<TUniqueId, boost::weak_ptr<MemTracker> > MemTrackersMap;
  static MemTrackersMap uid_to_mem_trackers_;
  static boost::mutex uid_to_mem_trackers_lock_;

  // Only valid for MemTrackers returned from GetMemTracker()
  TUniqueId query_id_;

  int64_t limit_;  // in bytes; < 0: no limit
  std::string label_;
  MemTracker* parent_;

  // in bytes; not owned
  RuntimeProfile::HighWaterMarkCounter* consumption_;

  // holds consumption_ counter if not tied to a profile
  RuntimeProfile::HighWaterMarkCounter local_counter_;

  // If non-NULL, used to measure consumption (in bytes) rather than the values provided
  // to Consume()/Release(). Only used for the process tracker, thus parent_ should be
  // NULL if consumption_metric_ is set.
  Metrics::PrimitiveMetric<uint64_t>* consumption_metric_;

  std::vector<MemTracker*> all_trackers_;  // this tracker plus all of its ancestors
  std::vector<MemTracker*> limit_trackers_;  // all_trackers_ with valid limits

  // All the child trackers of this tracker. Used for error reporting only.
  // i.e., Updating a parent tracker does not update the children.
  mutable boost::mutex child_trackers_lock_;
  std::list<MemTracker*> child_trackers_;

  // Iterator into parent_->child_trackers_ for this object. Stored to have O(1)
  // remove.
  std::list<MemTracker*>::iterator child_tracker_it_;

  // Functions to call after the limit is reached to free memory.
  std::vector<GcFunction> gc_functions_;

  // If true, calls UnregisterFromParent() in the dtor. This is only used for
  // the query wide trackers to remove it from the process mem tracker. The
  // process tracker never gets deleted so it is safe to reference it in the dtor.
  // The query tracker has lifetime shared by multiple plan fragments so it's hard
  // to do cleanup another way.
  bool auto_unregister_;

  // If true, logs to INFO every consume/release called. Used for debugging.
  bool enable_logging_;
  // If true, log the stack as well.
  bool log_stack_;

  // Walks the MemTracker hierarchy and populates all_trackers_ and
  // limit_trackers_
  void Init();

  // Adds tracker to child_trackers_
  void AddChildTracker(MemTracker* tracker);

  // Logs the stack of the current consume/release. Used for debugging only.
  void LogUpdate(bool is_consume, int64_t bytes) const;

  static std::string LogUsage(const std::string& prefix,
      const std::list<MemTracker*>& trackers);

  // The number of times the GcFunctions were called.
  Metrics::IntMetric* num_gcs_metric_;

  // The number of bytes freed by the last round of calling the GcFunctions (-1 before any
  // GCs are performed).
  Metrics::BytesMetric* bytes_freed_by_last_gc_metric_;

  // The number of bytes over the limit we were the last time LimitExceeded() was called
  // and the limit was exceeded pre-GC. -1 if there is no limit or the limit was never
  // exceeded.
  Metrics::BytesMetric* bytes_over_limit_metric_;
};

}

#endif

