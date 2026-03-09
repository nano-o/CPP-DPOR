#pragma once

// Program representation for DPOR exploration.
// A program is a collection of thread functions, each of which is a callback
// that returns the next event label given the trace of values observed so far
// (from receives via rf and ND choices, in program order) and the step count
// (number of events this thread has produced so far).
// The trace intentionally excludes send/block/error events, so trace length is
// not a program counter. The step parameter carries that control-state signal
// and allows send-only threads to know when to stop without relying on mutable
// captured state.

#include "dpor/model/event.hpp"

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dpor::algo {

template <typename T>
class ThreadMapT {
 public:
  ThreadMapT() = default;

  ThreadMapT(std::initializer_list<std::pair<model::ThreadId, T>> init) {
    for (const auto& [tid, value] : init) {
      (*this)[tid] = value;
    }
  }

  ThreadMapT& operator=(std::initializer_list<std::pair<model::ThreadId, T>> init) {
    clear();
    for (const auto& [tid, value] : init) {
      (*this)[tid] = value;
    }
    return *this;
  }

  [[nodiscard]] T& operator[](model::ThreadId tid) {
    const auto index = static_cast<std::size_t>(tid);
    if (index >= entries_.size()) {
      entries_.resize(index + 1);
    }
    if (!entries_[index].has_value()) {
      entries_[index].emplace();
      ++size_;
    }
    return *entries_[index];  // NOLINT(bugprone-unchecked-optional-access)
  }

  [[nodiscard]] const T& at(model::ThreadId tid) const {
    const auto index = static_cast<std::size_t>(tid);
    if (index >= entries_.size() || !entries_[index].has_value()) {
      throw std::out_of_range("thread id not found");
    }
    return *entries_[index];  // NOLINT(bugprone-unchecked-optional-access)
  }

  [[nodiscard]] bool contains(model::ThreadId tid) const noexcept {
    const auto index = static_cast<std::size_t>(tid);
    return index < entries_.size() && entries_[index].has_value();
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

  // Validate the completed thread set before exploration begins. We do not
  // enforce compactness while callers are still populating the map, because
  // registration order is independent of scheduling order.
  void validate_compact_thread_ids() const {
    if (size_ == 0) {
      return;
    }

    std::optional<model::ThreadId> min_tid;
    model::ThreadId max_tid = 0;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (!entries_[index].has_value()) {
        continue;
      }

      const auto tid = static_cast<model::ThreadId>(index);
      if (!min_tid.has_value() || tid < *min_tid) {
        min_tid = tid;
      }
      if (tid > max_tid) {
        max_tid = tid;
      }
    }

    const auto count = size_;
    const auto first_tid = *min_tid;  // NOLINT(bugprone-unchecked-optional-access)
    const bool zero_based = first_tid == 0 && static_cast<std::size_t>(max_tid) + 1U == count;
    const bool one_based = first_tid == 1 && static_cast<std::size_t>(max_tid) == count;
    if (zero_based || one_based) {
      return;
    }

    throw std::invalid_argument(
        "thread ids must form a compact contiguous 0-based or 1-based range; "
        "observed thread ids span [" +
        std::to_string(first_tid) + ", " + std::to_string(max_tid) + "] across " +
        std::to_string(count) + " assigned threads");
  }

  void clear() noexcept {
    entries_.clear();
    size_ = 0;
  }

  template <typename Fn>
  void for_each_assigned(const Fn& fn) const {
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (entries_[index].has_value()) {
        fn(static_cast<model::ThreadId>(index),
           *entries_[index]);  // NOLINT(bugprone-unchecked-optional-access)
      }
    }
  }

 private:
  std::vector<std::optional<T>> entries_;
  std::size_t size_{0};
};

template <typename ValueT>
using ThreadTraceEntryT = model::ObservedValueT<ValueT>;

template <typename ValueT>
using ThreadTraceT = std::vector<ThreadTraceEntryT<ValueT>>;

// Thread step function. Must be deterministic and side-effect-free: the same
// (trace, step) arguments must always produce the same event label. DPOR
// correctness (soundness and completeness) depends on this invariant.
// Stateful captures or external side effects will silently invalidate
// exploration guarantees.
// Because sends are not included in trace, callers must use `step` (not
// trace.size()) to determine local control-flow progress.
//
// Must-style blocking semantics: user thread functions should not return
// BlockLabel. DPOR injects Block events internally when a blocking receive has
// no currently compatible unread send.
template <typename ValueT>
using ThreadFunctionT = std::function<std::optional<model::EventLabelT<ValueT>>(
    const ThreadTraceT<ValueT>&, std::size_t step)>;

template <typename ValueT>
struct ProgramT {
  // Thread IDs are validated to form a compact contiguous 0-based or 1-based
  // range before exploration/oracle enumeration begins.
  ThreadMapT<ThreadFunctionT<ValueT>> threads;
};

using ThreadTrace = ThreadTraceT<model::Value>;
using ThreadTraceEntry = ThreadTraceEntryT<model::Value>;
using ThreadFunction = ThreadFunctionT<model::Value>;
using Program = ProgramT<model::Value>;

}  // namespace dpor::algo
