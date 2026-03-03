#pragma once

// Program representation for DPOR exploration.
// A program is a collection of thread functions, each of which is a callback
// that returns the next event label given the trace of values observed so far
// (from receives via rf and ND choices, in program order) and the step count
// (number of events this thread has produced so far).
// The step count allows send-only threads to know when to stop without
// relying on mutable captured state.

#include "dpor/model/event.hpp"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace dpor::algo {

template <typename ValueT>
using ThreadTraceT = std::vector<ValueT>;

// Thread step function. Must be deterministic and side-effect-free: the same
// (trace, step) arguments must always produce the same event label. DPOR
// correctness (soundness and completeness) depends on this invariant.
// Stateful captures or external side effects will silently invalidate
// exploration guarantees.
template <typename ValueT>
using ThreadFunctionT = std::function<
    std::optional<model::EventLabelT<ValueT>>(const ThreadTraceT<ValueT>&, std::size_t step)>;

template <typename ValueT>
struct ProgramT {
  std::unordered_map<model::ThreadId, ThreadFunctionT<ValueT>> threads;
};

using ThreadTrace = ThreadTraceT<model::Value>;
using ThreadFunction = ThreadFunctionT<model::Value>;
using Program = ProgramT<model::Value>;

}  // namespace dpor::algo
