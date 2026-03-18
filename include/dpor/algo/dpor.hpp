#pragma once

// DPOR exploration engine — Algorithm 1 from Enea et al., 2024.
//
// Given a program (collection of thread functions), explores all consistent
// execution graphs in a complete and optimal manner for the async communication
// model. Implements backward revisiting.
//
// All functions are header-only and templated on ValueT.

#include "dpor/algo/program.hpp"
#include "dpor/model/consistency.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <queue>
#include <ranges>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dpor::algo {

enum class VerifyResultKind : std::uint8_t { AllExecutionsExplored, ErrorFound, DepthLimitReached };

struct VerifyResult {
  VerifyResultKind kind{VerifyResultKind::AllExecutionsExplored};
  std::string message;
  std::size_t executions_explored{0};
};

template <typename ValueT>
using ExecutionObserverT = std::function<void(const model::ExplorationGraphT<ValueT>&)>;

template <typename ValueT>
struct DporConfigT {
  ProgramT<ValueT> program;
  std::size_t max_depth{1000};
  ExecutionObserverT<ValueT> on_execution{};
};

// Experimental options for verify_parallel(). A zero max_workers selects a
// hardware-based default; a zero max_queued_tasks derives a small queue budget
// from the resolved worker count.
struct ParallelVerifyOptions {
  std::size_t max_workers{0};
  std::size_t max_queued_tasks{0};
  std::size_t spawn_depth_cutoff{0};
  std::size_t min_fanout{2};
  // When non-zero, workers read the shared stop flag only every sync_steps
  // stop_requested() calls and batch execution counts in thread-local
  // accumulators.  This reduces contention at the cost of weaker error-stop
  // semantics: multiple workers may independently reach error terminals, and
  // complete executions may be counted after an error has been committed.
  // When zero (the default) the publication mutex serialises these paths so
  // that exactly one error is observed and no work is done after the stop.
  std::size_t sync_steps{0};
};

using DporConfig = DporConfigT<model::Value>;
using ExecutionObserver = ExecutionObserverT<model::Value>;

namespace detail {

using EventId = typename model::ExplorationGraphT<model::Value>::EventId;

[[nodiscard]] inline std::string format_error_message(const model::ThreadId tid,
                                                      const model::ErrorLabel& error) {
  auto message = "error event reached in thread " + std::to_string(tid);
  if (!error.message.empty()) {
    message += ": " + error.message;
  }
  return message;
}

template <typename ValueT>
[[nodiscard]] inline std::vector<model::ThreadId> sorted_thread_ids(
    const ProgramT<ValueT>& program) {
  program.threads.validate_compact_thread_ids();
  std::vector<model::ThreadId> thread_ids;
  thread_ids.reserve(program.threads.size());
  program.threads.for_each_assigned(
      [&](const model::ThreadId tid, const auto&) { thread_ids.push_back(tid); });
  return thread_ids;
}

template <typename ValueT>
[[nodiscard]] inline bool has_compatible_unread_send(const model::ExplorationGraphT<ValueT>& graph,
                                                     model::ThreadId tid,
                                                     const model::ReceiveLabelT<ValueT>& receive) {
  const auto unread_sends = graph.unread_send_event_ids();
  return std::ranges::any_of(unread_sends, [&](const auto send_id) {
    const auto* send = model::as_send(graph.event(send_id));
    return send != nullptr && send->destination == tid && receive.accepts(send->value);
  });
}

// Compute the next event to add to the graph, following Algorithm 1's next_P(G).
// Iterates threads by ascending ThreadId, calls the thread function with the
// current trace, skips blocked/done threads, and turns unsatisfied blocking
// receives into internal Block events.
template <typename ValueT>
[[nodiscard]] inline std::optional<std::pair<model::ThreadId, model::EventLabelT<ValueT>>>
compute_next_event(const ProgramT<ValueT>& program, const model::ExplorationGraphT<ValueT>& graph,
                   const std::vector<model::ThreadId>& thread_ids) {
  for (const auto tid : thread_ids) {
    // Skip threads that have terminated (block or error).
    if (graph.thread_is_terminated(tid)) {
      continue;
    }

    const auto& thread_fn = program.threads.at(tid);
    const auto trace = graph.thread_trace(tid);
    const auto step = graph.thread_event_count(tid);
    const auto next_label = thread_fn(trace, step);

    if (!next_label.has_value()) {
      continue;  // Thread is done.
    }

    const auto& label = *next_label;

    if (std::holds_alternative<model::BlockLabel>(label)) {
      throw std::logic_error(
          "thread function returned BlockLabel; Block events are internal to DPOR");
    }

    // If it's a receive, check if there's at least one compatible unread send.
    if (const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&label)) {
      if (recv->is_blocking() && !has_compatible_unread_send(graph, tid, *recv)) {
        // Must-style behavior: represent an unsatisfied blocking receive as a
        // Block event and continue with other threads.
        return std::pair{
            tid,
            model::EventLabelT<ValueT>{model::BlockLabel{}},
        };
      }
    }

    return std::pair{tid, label};
  }

  return std::nullopt;  // All threads blocked or done.
}

// Compute the "Previous" set: {e' ∈ G.E | e' ≤_G e ∨ ⟨e', s⟩ ∈ G.porf}.
template <typename ValueT>
[[nodiscard]] inline std::vector<std::uint8_t> compute_previous_set(
    const model::ExplorationGraphT<ValueT>& graph,
    typename model::ExplorationGraphT<ValueT>::EventId e,
    typename model::ExplorationGraphT<ValueT>::EventId s) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  const auto n = graph.event_count();
  std::vector<std::uint8_t> result(n, 0);
  for (EvId ep = 0; ep < n; ++ep) {
    // e' ≤_G e: ep was inserted before or at e.
    if (graph.inserted_before_or_equal(ep, e)) {
      result[ep] = 1;
      continue;
    }
    // ⟨e', s⟩ ∈ G.porf: ep reaches s through (po ∪ rf)+.
    if (graph.porf_contains(ep, s)) {
      result[ep] = 1;
    }
  }

  return result;
}

// Lightweight `(po ∪ rf)` view over a kept-event mask. This lets
// revisit_condition() emulate `G|Previous` without materializing a child graph.
template <typename ValueT>
class MaskedPorfContext {
 public:
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  MaskedPorfContext(const model::ExplorationGraphT<ValueT>& graph,
                    const std::vector<std::uint8_t>& keep_mask)
      : graph_(graph),
        keep_mask_(keep_mask),
        kept_position_in_thread_(graph.event_count(), kNoPosition),
        consumed_send_mask_(graph.event_count(), 0),
        rf_successors_by_send_(graph.event_count()) {
    if (keep_mask_.size() != graph_.event_count()) {
      throw std::invalid_argument("keep mask size must match event count");
    }

    for (const auto event_id : graph_.insertion_order()) {
      if (!keeps(event_id)) {
        continue;
      }
      const auto& evt = graph_.event(event_id);
      const auto thread_index = static_cast<std::size_t>(evt.thread);
      if (thread_index >= kept_events_by_thread_.size()) {
        kept_events_by_thread_.resize(thread_index + 1);
      }
      auto& thread_events = kept_events_by_thread_[thread_index];
      kept_position_in_thread_[event_id] = thread_events.size();
      thread_events.push_back(event_id);
    }

    for (const auto& [recv_id, source] : graph_.reads_from()) {
      if (!keeps(recv_id) || source.is_bottom()) {
        continue;
      }
      const auto send_id = source.send_id();
      if (!keeps(send_id)) {
        continue;
      }
      consumed_send_mask_[send_id] = 1U;
      rf_successors_by_send_[send_id].push_back(recv_id);
    }
  }

  [[nodiscard]] bool keeps(EvId event_id) const noexcept {
    return event_id < keep_mask_.size() && keep_mask_[event_id] != 0U;
  }

  [[nodiscard]] bool send_is_unread(EvId send_id) const noexcept {
    return keeps(send_id) && consumed_send_mask_[send_id] == 0U;
  }

  [[nodiscard]] std::vector<std::uint8_t> reachable_from(EvId from) const {
    if (!graph_.is_valid_event_id(from)) {
      throw std::out_of_range("event id not found in masked PORF context");
    }
    if (!keeps(from)) {
      throw std::logic_error("masked PORF reachability requires a kept source event");
    }

    std::vector<std::uint8_t> reachable(graph_.event_count(), 0);
    std::vector<EvId> pending;
    pending.reserve(graph_.event_count());
    append_successors(from, pending);

    std::size_t cursor = 0;
    while (cursor < pending.size()) {
      const auto current = pending[cursor++];
      if (reachable[current] != 0U) {
        continue;
      }
      reachable[current] = 1U;
      append_successors(current, pending);
    }

    return reachable;
  }

 private:
  static constexpr std::size_t kNoPosition = std::numeric_limits<std::size_t>::max();

  void append_successors(EvId from, std::vector<EvId>& pending) const {
    const auto thread_index = static_cast<std::size_t>(graph_.event(from).thread);
    if (thread_index < kept_events_by_thread_.size()) {
      const auto& thread_events = kept_events_by_thread_[thread_index];
      const auto position = kept_position_in_thread_[from];
      if (position != kNoPosition) {
        for (std::size_t i = position + 1; i < thread_events.size(); ++i) {
          pending.push_back(thread_events[i]);
        }
      }
    }

    for (const auto recv_id : rf_successors_by_send_[from]) {
      pending.push_back(recv_id);
    }
  }

  const model::ExplorationGraphT<ValueT>& graph_;
  const std::vector<std::uint8_t>& keep_mask_;
  std::vector<std::vector<EvId>> kept_events_by_thread_;
  std::vector<std::size_t> kept_position_in_thread_;
  std::vector<std::uint8_t> consumed_send_mask_;
  std::vector<std::vector<EvId>> rf_successors_by_send_;
};

template <typename ValueT>
[[nodiscard]] inline bool rewiring_recv_creates_cycle(
    const model::ExplorationGraphT<ValueT>& graph,
    typename model::ExplorationGraphT<ValueT>::EventId recv,
    typename model::ExplorationGraphT<ValueT>::EventId send) {
  // get_cons_tiebreaker() is evaluated on G|Previous while exploring already
  // consistent executions. restrict() clears the metadata bit, but removing
  // events cannot introduce a new causal cycle.
  assert(!graph.has_causal_cycle() && "get_cons_tiebreaker requires an acyclic graph");

  // Rewiring recv replaces its single inbound rf edge with send -> recv.
  // In an acyclic graph, removing the old inbound edge cannot create new
  // reachability from recv, so the rewritten graph is cyclic iff recv already
  // reaches send in the original graph.
  return graph.porf_contains(recv, send);
}

// GETCONSTIEBREAKER: for async, the tid-minimal send that is consistent
// (no cycle when assigned as rf source for recv).
template <typename ValueT>
[[nodiscard]] inline typename model::ExplorationGraphT<ValueT>::EventId get_cons_tiebreaker(
    const model::ExplorationGraphT<ValueT>& graph,
    typename model::ExplorationGraphT<ValueT>::EventId recv) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  const auto& recv_evt = graph.event(recv);
  const auto* recv_label = model::as_receive(recv_evt);
  if (recv_label == nullptr) {
    throw std::logic_error("get_cons_tiebreaker invariant violated: event is not a receive");
  }
  if (recv_label->is_nonblocking()) {
    throw std::logic_error(
        "get_cons_tiebreaker invariant violated: event is not a blocking receive");
  }

  // Find all compatible sends that are available for recv to read from:
  // unread sends plus the send that recv itself currently reads from (if any).
  struct Candidate {
    EvId send_id;
    model::ThreadId sender_thread;
  };

  // Determine which send recv currently reads from (if any).
  auto rf_it = graph.reads_from().find(recv);
  if (rf_it == graph.reads_from().end()) {
    throw std::logic_error(
        "get_cons_tiebreaker invariant violated: receive missing reads-from source");
  }
  if (rf_it->second.is_bottom()) {
    throw std::logic_error(
        "get_cons_tiebreaker invariant violated: blocking receive reads from bottom");
  }
  const auto current_rf_source = rf_it->second.send_id();

  std::vector<Candidate> candidates;
  for (const auto send_id : graph.unread_send_event_ids()) {
    const auto& send_evt = graph.event(send_id);
    const auto* send_label = model::as_send(send_evt);
    if (send_label == nullptr) {
      continue;
    }
    if (send_label->destination != recv_evt.thread) {
      continue;
    }
    if (!recv_label->accepts(send_label->value)) {
      continue;
    }
    candidates.push_back(Candidate{send_id, send_evt.thread});
  }

  // Also include the send that recv currently reads from (consumed by recv itself).
  const auto& send_evt = graph.event(current_rf_source);
  const auto* send_label = model::as_send(send_evt);
  if (send_label != nullptr && send_label->destination == recv_evt.thread &&
      recv_label->accepts(send_label->value)) {
    candidates.push_back(Candidate{current_rf_source, send_evt.thread});
  }

  // Sort by sender thread ID (tid-minimal), then by event ID for stability.
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.sender_thread != b.sender_thread) {
      return a.sender_thread < b.sender_thread;
    }
    return a.send_id < b.send_id;
  });

  // Return the first candidate that doesn't create a cycle.
  for (const auto& candidate : candidates) {
    if (!rewiring_recv_creates_cycle(graph, recv, candidate.send_id)) {
      return candidate.send_id;
    }
  }

  throw std::logic_error("get_cons_tiebreaker invariant violated: no consistent source found");
}

template <typename ValueT>
[[nodiscard]] inline typename model::ExplorationGraphT<ValueT>::EventId
get_cons_tiebreaker_masked(const model::ExplorationGraphT<ValueT>& graph,
                           const std::vector<std::uint8_t>& keep_mask,
                           typename model::ExplorationGraphT<ValueT>::EventId recv) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  const MaskedPorfContext<ValueT> masked_graph(graph, keep_mask);
  if (!masked_graph.keeps(recv)) {
    throw std::logic_error("get_cons_tiebreaker invariant violated: event missing from keep mask");
  }

  const auto& recv_evt = graph.event(recv);
  const auto* recv_label = model::as_receive(recv_evt);
  if (recv_label == nullptr) {
    throw std::logic_error("get_cons_tiebreaker invariant violated: event is not a receive");
  }
  if (recv_label->is_nonblocking()) {
    throw std::logic_error(
        "get_cons_tiebreaker invariant violated: event is not a blocking receive");
  }

  struct Candidate {
    EvId send_id;
    model::ThreadId sender_thread;
  };

  auto rf_it = graph.reads_from().find(recv);
  if (rf_it == graph.reads_from().end()) {
    throw std::logic_error(
        "get_cons_tiebreaker invariant violated: receive missing reads-from source");
  }
  if (rf_it->second.is_bottom()) {
    throw std::logic_error(
        "get_cons_tiebreaker invariant violated: blocking receive reads from bottom");
  }
  const auto current_rf_source = rf_it->second.send_id();
  if (!masked_graph.keeps(current_rf_source)) {
    throw std::logic_error(
        "get_cons_tiebreaker invariant violated: receive source missing from keep mask");
  }

  std::vector<Candidate> candidates;
  candidates.reserve(graph.event_count());
  for (EvId send_id = 0; send_id < graph.event_count(); ++send_id) {
    if (!masked_graph.send_is_unread(send_id)) {
      continue;
    }
    const auto& send_evt = graph.event(send_id);
    const auto* send_label = model::as_send(send_evt);
    if (send_label == nullptr) {
      continue;
    }
    if (send_label->destination != recv_evt.thread) {
      continue;
    }
    if (!recv_label->accepts(send_label->value)) {
      continue;
    }
    candidates.push_back(Candidate{send_id, send_evt.thread});
  }

  const auto& send_evt = graph.event(current_rf_source);
  const auto* send_label = model::as_send(send_evt);
  if (send_label != nullptr && send_label->destination == recv_evt.thread &&
      recv_label->accepts(send_label->value)) {
    candidates.push_back(Candidate{current_rf_source, send_evt.thread});
  }

  std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
    if (a.sender_thread != b.sender_thread) {
      return a.sender_thread < b.sender_thread;
    }
    return a.send_id < b.send_id;
  });

  const auto reachable_from_recv = masked_graph.reachable_from(recv);
  for (const auto& candidate : candidates) {
    if (reachable_from_recv[candidate.send_id] == 0U) {
      return candidate.send_id;
    }
  }

  throw std::logic_error("get_cons_tiebreaker invariant violated: no consistent source found");
}

// REVISITCONDITION(G, e, s):
// - ND → val(e) == min(S)
// - non-receive → no receive in Previous reads from e
// - receive → rf(e) == get_cons_tiebreaker(G|Previous, e)
template <typename ValueT>
[[nodiscard]] inline bool revisit_condition(const model::ExplorationGraphT<ValueT>& graph,
                                            typename model::ExplorationGraphT<ValueT>::EventId e,
                                            typename model::ExplorationGraphT<ValueT>::EventId s) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;
  constexpr auto kNoSource = model::ExplorationGraphT<ValueT>::kNoSource;

  const auto& evt = graph.event(e);

  if (const auto* recv = model::as_receive(evt); recv != nullptr && recv->is_nonblocking()) {
    auto rf_it = graph.reads_from().find(e);
    if (rf_it == graph.reads_from().end()) {
      throw std::logic_error(
          "revisit_condition invariant violated: non-blocking receive missing reads-from source");
    }
    return rf_it->second.is_bottom();
  }

  // ND choice: val(e) == min(S).
  // NOTE: This branch requires ValueT to provide operator< (for min_element)
  // and operator== (for comparison). If ValueT lacks these, verify<T>() will
  // fail to compile even when no ND events are produced at runtime.
  if (const auto* nd = model::as_nondeterministic_choice(evt)) {
    if (nd->choices.empty()) {
      return true;
    }
    auto min_it = std::min_element(nd->choices.begin(), nd->choices.end());
    return nd->value == *min_it;
  }

  // Non-receive event (send, block, error): check that no receive in Previous
  // reads from e.
  if (!model::is_receive(evt)) {
    const auto previous = compute_previous_set(graph, e, s);
    for (EvId ep = 0; ep < previous.size(); ++ep) {
      if (previous[ep] == 0U) {
        continue;
      }
      if (model::is_receive(graph.event(ep))) {
        auto it = graph.reads_from().find(ep);
        if (it != graph.reads_from().end() && it->second.is_send() && it->second.send_id() == e) {
          return false;  // A receive in Previous reads from e.
        }
      }
    }
    return true;
  }

  // Receive: rf(e) == get_cons_tiebreaker(G|Previous, e)
  // Must Algorithm 1 requires the tiebreaker to be computed on G restricted
  // to the Previous set, not the full graph.
  const auto previous = compute_previous_set(graph, e, s);
  if (previous[e] == 0U) {
    throw std::logic_error("revisit_condition invariant violated: event missing from Previous");
  }

  // If rf(e) is not in Previous, the equality must fail: it cannot match any
  // tiebreaker source of G|Previous.
  auto rf_it = graph.reads_from().find(e);
  if (rf_it == graph.reads_from().end()) {
    throw std::logic_error(
        "revisit_condition invariant violated: receive missing reads-from source");
  }
  if (rf_it->second.is_bottom()) {
    throw std::logic_error(
        "revisit_condition invariant violated: blocking receive reads from bottom");
  }
  const auto current_rf_original = rf_it->second.send_id();
  if (current_rf_original == kNoSource || current_rf_original >= previous.size() ||
      previous[current_rf_original] == 0U) {
    // This is a normal blocking-receive failure case, not an invariant break.
    return false;
  }

  const auto tiebreaker = get_cons_tiebreaker_masked(graph, previous, e);
  return current_rf_original == tiebreaker;
}

enum class ExplorationTaskMode : std::uint8_t { Visit, VisitIfConsistent };

template <typename ValueT>
struct ExplorationTask {
  model::ExplorationGraphT<ValueT> graph;
  std::size_t depth{0};
  ExplorationTaskMode mode{ExplorationTaskMode::Visit};
};

template <typename ValueT, typename ExecutorT>
inline void visit_impl(const ProgramT<ValueT>& program, model::ExplorationGraphT<ValueT>& graph,
                       ExecutorT& executor, const DporConfigT<ValueT>& config, std::size_t depth,
                       const std::vector<model::ThreadId>& thread_ids);

template <typename ValueT, typename ExecutorT>
inline void visit_if_consistent_impl(const ProgramT<ValueT>& program,
                                     model::ExplorationGraphT<ValueT>& graph, ExecutorT& executor,
                                     const DporConfigT<ValueT>& config, std::size_t depth,
                                     const std::vector<model::ThreadId>& thread_ids);

template <typename ValueT>
class SequentialExecutor {
 public:
  SequentialExecutor(VerifyResult& result, const DporConfigT<ValueT>& config)
      : result_(result), config_(config) {}

  [[nodiscard]] bool stop_requested() const noexcept {
    return result_.kind == VerifyResultKind::ErrorFound;
  }

  void note_depth_limit() {
    if (result_.kind == VerifyResultKind::AllExecutionsExplored) {
      result_.kind = VerifyResultKind::DepthLimitReached;
    }
  }

  [[nodiscard]] bool can_spawn(std::size_t /*depth*/, std::size_t /*fanout*/) const noexcept {
    return false;
  }

  [[nodiscard]] bool try_enqueue(ExplorationTask<ValueT>& /*task*/) const noexcept { return false; }

  [[nodiscard]] bool publish_complete_execution(const model::ExplorationGraphT<ValueT>& graph) {
    ++result_.executions_explored;
    if (config_.on_execution) {
      config_.on_execution(graph);
    }
    return true;
  }

  [[nodiscard]] bool publish_error_execution(const model::ExplorationGraphT<ValueT>& graph,
                                             const model::ThreadId tid,
                                             const model::ErrorLabel& error) {
    ++result_.executions_explored;
    result_.kind = VerifyResultKind::ErrorFound;
    result_.message = format_error_message(tid, error);
    if (config_.on_execution) {
      config_.on_execution(graph);
    }
    return true;
  }

 private:
  VerifyResult& result_;
  const DporConfigT<ValueT>& config_;
};

template <typename ValueT>
class ParallelExecutor {
 public:
  ParallelExecutor(const DporConfigT<ValueT>& config, ParallelVerifyOptions options,
                   std::vector<model::ThreadId> thread_ids)
      : config_(config),
        options_(options),
        thread_ids_(std::move(thread_ids)),
        max_workers_(resolve_max_workers(options.max_workers)),
        max_queued_tasks_(resolve_max_queued_tasks(options.max_queued_tasks, max_workers_)),
        min_fanout_(std::max<std::size_t>(1, options.min_fanout)),
        sync_steps_(options.sync_steps) {}

  [[nodiscard]] VerifyResult run() {
    {
      std::lock_guard lock(queue_mutex_);
      task_queue_.push(ExplorationTask<ValueT>{
          .graph = model::ExplorationGraphT<ValueT>{},
          .depth = 0,
          .mode = ExplorationTaskMode::Visit,
      });
    }

    std::vector<std::thread> workers;
    workers.reserve(max_workers_ > 0 ? max_workers_ - 1U : 0U);
    for (std::size_t worker_index = 1; worker_index < max_workers_; ++worker_index) {
      workers.emplace_back([this]() { worker_loop(); });
    }

    worker_loop();

    for (auto& worker : workers) {
      worker.join();
    }

    // All workers have joined; no synchronisation needed.
    if (first_exception_) {
      std::rethrow_exception(first_exception_);
    }

    VerifyResult result;
    result.executions_explored = executions_explored_.load(std::memory_order_relaxed);
    if (first_error_message_.has_value()) {
      result.kind = VerifyResultKind::ErrorFound;
      result.message = *first_error_message_;
    } else if (depth_limit_reached_.load(std::memory_order_relaxed)) {
      result.kind = VerifyResultKind::DepthLimitReached;
    }
    return result;
  }

  [[nodiscard]] bool stop_requested() noexcept {
    if (sync_steps_ == 0) {
      return stop_requested_.load(std::memory_order_acquire);
    }
    auto& state = worker_state();
    if (++state.steps_since_sync >= sync_steps_) {
      state.steps_since_sync = 0;
      state.cached_stop = stop_requested_.load(std::memory_order_acquire);
    }
    return state.cached_stop;
  }

  void note_depth_limit() noexcept { depth_limit_reached_.store(true, std::memory_order_relaxed); }

  [[nodiscard]] bool can_spawn(const std::size_t child_depth, const std::size_t fanout) noexcept {
    if (max_workers_ <= 1 || stop_requested()) {
      return false;
    }
    if (fanout < min_fanout_) {
      return false;
    }
    if (options_.spawn_depth_cutoff != 0 && child_depth > options_.spawn_depth_cutoff) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool try_enqueue(ExplorationTask<ValueT>& task) {
    if (max_workers_ <= 1 || stop_requested()) {
      return false;
    }

    bool enqueued = false;
    {
      std::lock_guard lock(queue_mutex_);
      if (!stop_requested_.load(std::memory_order_relaxed) && !search_complete_ &&
          task_queue_.size() < max_queued_tasks_) {
        task_queue_.push(std::move(task));
        enqueued = true;
      }
    }

    if (enqueued) {
      queue_cv_.notify_one();
    }
    return enqueued;
  }

  [[nodiscard]] bool publish_complete_execution(const model::ExplorationGraphT<ValueT>& graph) {
    if (sync_steps_ == 0) {
      // Serialise with publish_error_execution so that no complete execution
      // is counted or observed after an error has been committed.
      std::lock_guard lock(publication_mutex_);
      if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
      }
      ++worker_state().local_executions;
    } else {
      if (stop_requested()) {
        return false;
      }
      ++worker_state().local_executions;
    }

    if (config_.on_execution) {
      config_.on_execution(graph);
    }
    return true;
  }

  [[nodiscard]] bool publish_error_execution(const model::ExplorationGraphT<ValueT>& graph,
                                             const model::ThreadId tid,
                                             const model::ErrorLabel& error) {
    bool published = false;
    {
      std::lock_guard lock(publication_mutex_);
      if (sync_steps_ == 0) {
        // Strict mode: only the first error is counted and observed.
        if (stop_requested_.load(std::memory_order_acquire)) {
          return false;
        }
        ++worker_state().local_executions;
        first_error_message_ = format_error_message(tid, error);
        stop_requested_.store(true, std::memory_order_release);
        published = true;
      } else {
        // Relaxed mode: every worker that independently reaches an error
        // is counted and observed; only the first message is kept.
        ++worker_state().local_executions;
        if (!first_error_message_.has_value()) {
          first_error_message_ = format_error_message(tid, error);
        }
        stop_requested_.store(true, std::memory_order_release);
        published = true;
      }
    }

    if (published && config_.on_execution) {
      config_.on_execution(graph);
    }
    queue_cv_.notify_all();
    return published;
  }

 private:
  [[nodiscard]] static std::size_t resolve_max_workers(const std::size_t requested) {
    if (requested != 0) {
      return requested;
    }
    const auto concurrency = std::thread::hardware_concurrency();
    return concurrency == 0 ? 1U : static_cast<std::size_t>(concurrency);
  }

  [[nodiscard]] static std::size_t resolve_max_queued_tasks(const std::size_t requested,
                                                            const std::size_t max_workers) {
    if (requested != 0) {
      return requested;
    }
    return std::max<std::size_t>(1, max_workers * 2U);
  }

  void worker_loop() {
    while (true) {
      std::optional<ExplorationTask<ValueT>> task;
      {
        std::unique_lock lock(queue_mutex_);
        queue_cv_.wait(lock, [this]() {
          return stop_requested_.load(std::memory_order_acquire) || search_complete_ ||
                 !task_queue_.empty();
        });

        if (stop_requested_.load(std::memory_order_acquire) || search_complete_) {
          flush_worker_state();
          return;
        }

        task.emplace(std::move(task_queue_.front()));
        task_queue_.pop();
        ++active_workers_;
      }

      try {
        process_task(std::move(*task));
      } catch (...) {
        record_exception(std::current_exception());
      }

      {
        std::lock_guard lock(queue_mutex_);
        --active_workers_;
        if (!stop_requested_.load(std::memory_order_acquire) && task_queue_.empty() &&
            active_workers_ == 0) {
          search_complete_ = true;
        }
      }
      queue_cv_.notify_all();
    }
  }

  void process_task(ExplorationTask<ValueT> task) {
    if (stop_requested()) {
      return;
    }

    if (task.mode == ExplorationTaskMode::VisitIfConsistent) {
      visit_if_consistent_impl(config_.program, task.graph, *this, config_, task.depth,
                               thread_ids_);
      return;
    }

    visit_impl(config_.program, task.graph, *this, config_, task.depth, thread_ids_);
  }

  struct WorkerState {
    std::size_t local_executions{0};
    std::size_t steps_since_sync{0};
    bool cached_stop{false};
  };

  WorkerState& worker_state() noexcept {
    thread_local WorkerState state;
    return state;
  }

  void flush_worker_state() {
    auto& state = worker_state();
    if (state.local_executions > 0) {
      executions_explored_.fetch_add(state.local_executions, std::memory_order_relaxed);
    }
    state = WorkerState{};
  }

  void record_exception(std::exception_ptr exception) {
    {
      std::lock_guard lock(publication_mutex_);
      if (!first_exception_) {
        first_exception_ = std::move(exception);
      }
      stop_requested_.store(true, std::memory_order_release);
    }
    queue_cv_.notify_all();
  }

  const DporConfigT<ValueT>& config_;
  ParallelVerifyOptions options_;
  std::vector<model::ThreadId> thread_ids_;
  std::size_t max_workers_{1};
  std::size_t max_queued_tasks_{1};
  std::size_t min_fanout_{1};
  std::size_t sync_steps_{0};

  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> depth_limit_reached_{false};
  std::atomic<std::size_t> executions_explored_{0};

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::queue<ExplorationTask<ValueT>> task_queue_;
  std::size_t active_workers_{0};
  bool search_complete_{false};

  mutable std::mutex publication_mutex_;
  std::optional<std::string> first_error_message_;
  std::exception_ptr first_exception_;
};

template <typename ValueT, typename ExecutorT>
inline void recurse_graph(const ProgramT<ValueT>& program, model::ExplorationGraphT<ValueT>& graph,
                          ExecutorT& executor, const DporConfigT<ValueT>& config,
                          const std::size_t depth, const std::vector<model::ThreadId>& thread_ids,
                          const ExplorationTaskMode mode) {
  if (mode == ExplorationTaskMode::VisitIfConsistent) {
    visit_if_consistent_impl(program, graph, executor, config, depth, thread_ids);
    return;
  }
  visit_impl(program, graph, executor, config, depth, thread_ids);
}

template <typename ValueT, typename ExecutorT>
inline void process_owned_task(const ProgramT<ValueT>& program,
                               model::ExplorationGraphT<ValueT> graph, ExecutorT& executor,
                               const DporConfigT<ValueT>& config, const std::size_t depth,
                               const std::vector<model::ThreadId>& thread_ids,
                               const ExplorationTaskMode mode) {
  recurse_graph(program, graph, executor, config, depth, thread_ids, mode);
}

template <typename ValueT, typename ExecutorT>
[[nodiscard]] inline bool try_enqueue_owned_task(ExecutorT& executor,
                                                 model::ExplorationGraphT<ValueT>& graph,
                                                 const std::size_t depth,
                                                 const ExplorationTaskMode mode) {
  ExplorationTask<ValueT> task{
      .graph = std::move(graph),
      .depth = depth,
      .mode = mode,
  };
  if (executor.try_enqueue(task)) {
    return true;
  }
  graph = std::move(task.graph);
  return false;
}

// Explore a single child of an ND or receive branch locally.  The first child
// reuses the parent graph via ScopedRollback; subsequent children do the same.
template <typename ValueT, typename ExecutorT, typename MutateFn>
inline void explore_branch(const ProgramT<ValueT>& program,
                           model::ExplorationGraphT<ValueT>& parent_graph, ExecutorT& executor,
                           const DporConfigT<ValueT>& config, const std::size_t depth,
                           const std::vector<model::ThreadId>& thread_ids,
                           const ExplorationTaskMode mode, MutateFn&& mutate_branch) {
  using ScopedRollback = typename model::ExplorationGraphT<ValueT>::ScopedRollback;

  ScopedRollback rollback(parent_graph);
  std::forward<MutateFn>(mutate_branch)(parent_graph);
  recurse_graph(program, parent_graph, executor, config, depth, thread_ids, mode);
}

template <typename ValueT, typename StopFn, typename EmitFn>
inline void for_each_backward_revisit_child(
    const model::ExplorationGraphT<ValueT>& graph,
    const typename model::ExplorationGraphT<ValueT>::EventId send_id,
    StopFn&& should_stop,       // NOLINT(cppcoreguidelines-missing-std-forward)
    EmitFn&& emit_revisited) {  // NOLINT(cppcoreguidelines-missing-std-forward)
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  // Backward revisiting: Algorithm 1 lines 10-13. For each receive in the
  // destination thread of the new send, check compatibility and revisit
  // conditions, then emit an owned restricted+rewired child graph.
  if (should_stop()) {
    return;
  }

  const auto& send_evt = graph.event(send_id);
  const auto* send_label = model::as_send(send_evt);
  if (send_label == nullptr) {
    return;
  }

  const auto receives = graph.receives_in_destination(send_id);

  for (const auto recv_id : receives) {
    if (should_stop()) {
      return;
    }

    const auto& recv_evt = graph.event(recv_id);
    const auto* recv_label = model::as_receive(recv_evt);
    if (recv_label == nullptr || !recv_label->accepts(send_label->value)) {
      continue;
    }

    if (graph.porf_contains(recv_id, send_id)) {
      continue;
    }

    std::vector<std::uint8_t> deleted(graph.event_count(), 0);
    for (EvId ep = 0; ep < graph.event_count(); ++ep) {
      if (ep == recv_id || ep == send_id) {
        continue;
      }
      if (graph.inserted_before_or_equal(ep, recv_id)) {
        continue;
      }
      if (!graph.porf_contains(ep, send_id)) {
        deleted[ep] = 1;
      }
    }

    if (!revisit_condition(graph, recv_id, send_id)) {
      continue;
    }
    bool all_pass = true;
    for (EvId ep = 0; ep < deleted.size(); ++ep) {
      if (deleted[ep] == 0U) {
        continue;
      }
      if (!revisit_condition(graph, ep, send_id)) {
        all_pass = false;
        break;
      }
    }
    if (!all_pass) {
      continue;
    }

    std::vector<std::uint8_t> keep_mask(graph.event_count(), 1);
    for (EvId ep = 0; ep < graph.event_count(); ++ep) {
      if (deleted[ep] != 0U) {
        keep_mask[ep] = 0;
      }
    }

    auto restricted = model::detail::restrict_masked(graph, keep_mask);

    EvId new_recv_id = model::ExplorationGraphT<ValueT>::kNoSource;
    EvId new_send_id = model::ExplorationGraphT<ValueT>::kNoSource;
    EvId new_id = 0;
    for (const auto old_id : graph.insertion_order()) {
      if (keep_mask[old_id] == 0U) {
        continue;
      }
      if (old_id == recv_id) {
        new_recv_id = new_id;
      }
      if (old_id == send_id) {
        new_send_id = new_id;
      }
      ++new_id;
    }

    if (new_recv_id == model::ExplorationGraphT<ValueT>::kNoSource ||
        new_send_id == model::ExplorationGraphT<ValueT>::kNoSource) {
      continue;
    }

    restricted.rebind_rf_preserving_known_acyclicity(new_recv_id, new_send_id);
    auto revisited = std::move(restricted);
    if (!emit_revisited(std::move(revisited))) {
      return;
    }
  }
}

template <typename ValueT, typename ExecutorT>
[[nodiscard]] inline bool reschedule_blocked_receive_if_enabled_impl(
    const ProgramT<ValueT>& program, const model::ExplorationGraphT<ValueT>& graph,
    ExecutorT& executor, const DporConfigT<ValueT>& config, const std::size_t depth,
    const std::vector<model::ThreadId>& thread_ids) {
  constexpr auto kNoSource = model::ExplorationGraphT<ValueT>::kNoSource;

  for (const auto tid : thread_ids) {
    if (executor.stop_requested()) {
      return true;
    }

    const auto last_id = graph.last_event_id(tid);
    if (last_id == kNoSource || !model::is_block(graph.event(last_id))) {
      continue;
    }

    std::vector<std::uint8_t> keep_mask(graph.event_count(), 1);
    keep_mask[last_id] = 0;
    auto unblocked_graph = model::detail::restrict_masked(graph, keep_mask);

    const auto& thread_fn = program.threads.at(tid);
    const auto trace = unblocked_graph.thread_trace(tid);
    const auto step = unblocked_graph.thread_event_count(tid);
    const auto next_label = thread_fn(trace, step);

    if (!next_label.has_value()) {
      throw std::logic_error(
          "blocked thread became done after unblocking; expected a blocking receive");
    }
    if (std::holds_alternative<model::BlockLabel>(*next_label)) {
      throw std::logic_error(
          "thread function returned BlockLabel; Block events are internal to DPOR");
    }

    const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&*next_label);
    if (recv == nullptr) {
      throw std::logic_error("blocked thread did not produce a receive after unblocking");
    }
    if (recv->is_nonblocking()) {
      throw std::logic_error("blocked thread produced a non-blocking receive after unblocking");
    }
    if (!has_compatible_unread_send(unblocked_graph, tid, *recv)) {
      continue;
    }

    process_owned_task(program, std::move(unblocked_graph), executor, config, depth, thread_ids,
                       ExplorationTaskMode::Visit);
    return true;
  }

  return false;
}

template <typename ValueT, typename ExecutorT>
inline void visit_if_consistent_impl(const ProgramT<ValueT>& program,
                                     model::ExplorationGraphT<ValueT>& graph, ExecutorT& executor,
                                     const DporConfigT<ValueT>& config, const std::size_t depth,
                                     const std::vector<model::ThreadId>& thread_ids) {
  if (executor.stop_requested()) {
    return;
  }

  model::AsyncConsistencyCheckerT<ValueT> checker;
  const auto consistency = checker.check(graph);
  if (!consistency.is_consistent()) {
    return;
  }

  visit_impl(program, graph, executor, config, depth, thread_ids);
}

template <typename ValueT, typename ExecutorT>
inline void visit_impl(const ProgramT<ValueT>& program, model::ExplorationGraphT<ValueT>& graph,
                       ExecutorT& executor, const DporConfigT<ValueT>& config,
                       const std::size_t depth, const std::vector<model::ThreadId>& thread_ids) {
  using ScopedRollback = typename model::ExplorationGraphT<ValueT>::ScopedRollback;

  if (executor.stop_requested()) {
    return;
  }

  if (depth >= config.max_depth) {
    executor.note_depth_limit();
    return;
  }

  const auto next = compute_next_event(program, graph, thread_ids);

  if (!next.has_value()) {
    if (reschedule_blocked_receive_if_enabled_impl(program, graph, executor, config, depth,
                                                   thread_ids)) {
      return;
    }
    static_cast<void>(executor.publish_complete_execution(graph));
    return;
  }

  const auto& [tid, label] = *next;

  if (const auto* error = std::get_if<model::ErrorLabel>(&label)) {
    ScopedRollback rollback(graph);
    static_cast<void>(graph.add_event(tid, label));
    static_cast<void>(executor.publish_error_execution(graph, tid, *error));
    return;
  }

  if (const auto* nd = std::get_if<model::NondeterministicChoiceLabelT<ValueT>>(&label)) {
    if (nd->choices.empty()) {
      ScopedRollback rollback(graph);
      static_cast<void>(graph.add_event(tid, label));
      visit_impl(program, graph, executor, config, depth + 1, thread_ids);
      return;
    }

    for (const auto& choice : nd->choices) {
      if (executor.stop_requested()) {
        return;
      }
      auto nd_label = *nd;
      nd_label.value = choice;
      explore_branch(
          program, graph, executor, config, depth + 1, thread_ids, ExplorationTaskMode::Visit,
          [tid, nd_label = std::move(nd_label)](auto& branch_graph) {
            static_cast<void>(branch_graph.add_event(tid, model::EventLabelT<ValueT>{nd_label}));
          });
    }
    return;
  }

  if (const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&label)) {
    std::vector<typename model::ExplorationGraphT<ValueT>::EventId> compatible_sends;
    for (const auto send_id : graph.unread_send_event_ids()) {
      const auto* send = model::as_send(graph.event(send_id));
      if (send != nullptr && send->destination == tid && recv->accepts(send->value)) {
        compatible_sends.push_back(send_id);
      }
    }

    for (const auto send_id : compatible_sends) {
      if (executor.stop_requested()) {
        return;
      }
      explore_branch(program, graph, executor, config, depth + 1, thread_ids,
                     ExplorationTaskMode::VisitIfConsistent,
                     [tid, &label, send_id](auto& branch_graph) {
                       const auto recv_id = branch_graph.add_event(tid, label);
                       branch_graph.set_reads_from(recv_id, send_id);
                     });
    }

    if (recv->is_nonblocking() && !executor.stop_requested()) {
      explore_branch(program, graph, executor, config, depth + 1, thread_ids,
                     ExplorationTaskMode::VisitIfConsistent, [tid, &label](auto& branch_graph) {
                       const auto recv_id = branch_graph.add_event(
                           tid, label);  // NOLINT(clang-analyzer-core.NullDereference)
                       branch_graph.set_reads_from_bottom(recv_id);
                     });
    }
    return;
  }

  if (std::holds_alternative<model::SendLabelT<ValueT>>(label)) {
    ScopedRollback rollback(graph);
    const auto send_id = graph.add_event(tid, label);
    const auto allow_enqueue = executor.can_spawn(depth + 1, 2);

    for_each_backward_revisit_child(
        graph, send_id, [&executor]() { return executor.stop_requested(); },
        [&](model::ExplorationGraphT<ValueT> revisited) {
          if (allow_enqueue &&
              try_enqueue_owned_task<ValueT>(executor, revisited, depth + 1,
                                             ExplorationTaskMode::VisitIfConsistent)) {
            return true;
          }

          process_owned_task(program, std::move(revisited), executor, config, depth + 1, thread_ids,
                             ExplorationTaskMode::VisitIfConsistent);
          return !executor.stop_requested();
        });

    if (!executor.stop_requested()) {
      visit_impl(program, graph, executor, config, depth + 1, thread_ids);
    }
    return;
  }

  if (std::holds_alternative<model::BlockLabel>(label)) {
    ScopedRollback rollback(graph);
    static_cast<void>(graph.add_event(tid, label));
    visit_impl(program, graph, executor, config, depth + 1, thread_ids);
    return;
  }
}

template <typename ValueT>
inline void visit_if_consistent(const ProgramT<ValueT>& program,
                                model::ExplorationGraphT<ValueT>& graph, VerifyResult& result,
                                const DporConfigT<ValueT>& config, std::size_t depth,
                                const std::vector<model::ThreadId>& thread_ids) {
  SequentialExecutor<ValueT> executor(result, config);
  visit_if_consistent_impl(program, graph, executor, config, depth, thread_ids);
}

template <typename ValueT>
inline void backward_revisit(const ProgramT<ValueT>& program,
                             const model::ExplorationGraphT<ValueT>& graph,
                             typename model::ExplorationGraphT<ValueT>::EventId send_id,
                             VerifyResult& result, const DporConfigT<ValueT>& config,
                             std::size_t depth, const std::vector<model::ThreadId>& thread_ids) {
  SequentialExecutor<ValueT> executor(result, config);
  for_each_backward_revisit_child(
      graph, send_id, [&executor]() { return executor.stop_requested(); },
      [&](model::ExplorationGraphT<ValueT> revisited) {
        process_owned_task(program, std::move(revisited), executor, config, depth, thread_ids,
                           ExplorationTaskMode::VisitIfConsistent);
        return !executor.stop_requested();
      });
}

template <typename ValueT>
inline void visit(const ProgramT<ValueT>& program, model::ExplorationGraphT<ValueT>& graph,
                  VerifyResult& result, const DporConfigT<ValueT>& config, std::size_t depth,
                  const std::vector<model::ThreadId>& thread_ids) {
  SequentialExecutor<ValueT> executor(result, config);
  visit_impl(program, graph, executor, config, depth, thread_ids);
}

}  // namespace detail

// VERIFY(P): entry point. Creates empty G₀, calls visit.
template <typename ValueT>
[[nodiscard]] inline VerifyResult verify(const DporConfigT<ValueT>& config) {
  VerifyResult result;
  model::ExplorationGraphT<ValueT> empty_graph;
  const auto thread_ids = detail::sorted_thread_ids(config.program);
  detail::visit(config.program, empty_graph, result, config, 0, thread_ids);
  return result;
}

template <typename ValueT>
[[nodiscard]] inline VerifyResult verify_parallel(const DporConfigT<ValueT>& config,
                                                  ParallelVerifyOptions options = {}) {
  // Experimental.  With sync_steps=0 (default), exactly one error terminal is
  // counted and observed; callback order among successful executions is
  // unspecified.  With sync_steps>0, multiple workers may independently reach
  // error terminals before the stop signal propagates.
  const auto thread_ids = detail::sorted_thread_ids(config.program);
  detail::ParallelExecutor<ValueT> executor(config, options, thread_ids);
  return executor.run();
}

}  // namespace dpor::algo
