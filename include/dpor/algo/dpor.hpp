#pragma once

// DPOR exploration engine — Algorithm 1 from Enea et al., 2024.
//
// Given a program (collection of thread functions), explores all consistent
// execution graphs in a complete and optimal manner for the configured
// communication model. Implements backward revisiting.
//
// All functions are header-only and templated on ValueT.

#include "dpor/algo/program.hpp"
#include "dpor/model/consistency.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
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
#include <type_traits>
#include <utility>
#include <vector>

namespace dpor::algo {

enum class VerifyResultKind : std::uint8_t {
  AllExplored,
  Stopped,
  AllExecutionsExplored = AllExplored,
};
enum class ProgressState : std::uint8_t { Running, Stopped, AllExplored };
enum class TerminalExecutionKind : std::uint8_t { Full, Error, DepthLimit };
enum class TerminalExecutionAction : std::uint8_t { Continue, Stop };

struct VerifyResult {
  VerifyResultKind kind{VerifyResultKind::AllExplored};
  std::size_t executions_explored{0};
  std::size_t full_executions_explored{0};
  std::size_t error_executions_explored{0};
  std::size_t depth_limit_executions_explored{0};

  [[nodiscard]] bool all_explored() const noexcept { return kind == VerifyResultKind::AllExplored; }

  [[nodiscard]] bool stopped() const noexcept { return kind == VerifyResultKind::Stopped; }

  [[nodiscard]] std::size_t terminal_executions_explored() const noexcept {
    return executions_explored;
  }
};

struct ProgressSnapshot {
  ProgressState state{ProgressState::Running};
  std::chrono::steady_clock::duration elapsed{};
  std::size_t terminal_executions{0};
  std::size_t full_executions{0};
  std::size_t error_executions{0};
  std::size_t depth_limit_executions{0};
  std::size_t active_workers{0};
  std::size_t max_workers{1};
  std::size_t queued_tasks{0};
  std::size_t max_queued_tasks{0};
  bool counts_exact{true};
};

using ProgressObserver = std::function<void(const ProgressSnapshot&)>;

template <typename ValueT>
struct TerminalExecutionT {
  const model::ExplorationGraphT<ValueT>& graph;
  TerminalExecutionKind kind;

  [[nodiscard]] bool is_full_execution() const noexcept {
    return kind == TerminalExecutionKind::Full;
  }

  [[nodiscard]] bool is_error_execution() const noexcept {
    return kind == TerminalExecutionKind::Error;
  }

  [[nodiscard]] bool is_depth_limit_execution() const noexcept {
    return kind == TerminalExecutionKind::DepthLimit;
  }

  operator const model::ExplorationGraphT<ValueT>&() const noexcept { return graph; }
};

template <typename ValueT>
// Observers are called for every published terminal execution: full
// executions, error executions, and branches truncated by max_depth. Returning
// Stop requests early termination; void callbacks are treated as Continue.
class TerminalExecutionObserverT {
 public:
  using Execution = TerminalExecutionT<ValueT>;
  using Handler = std::function<TerminalExecutionAction(const Execution&)>;

  TerminalExecutionObserverT() = default;
  TerminalExecutionObserverT(std::nullptr_t) noexcept {}

  template <typename Fn,
            std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, TerminalExecutionObserverT>, int> = 0>
  TerminalExecutionObserverT(Fn&& fn) {
    assign(std::forward<Fn>(fn));
  }

  TerminalExecutionObserverT& operator=(std::nullptr_t) noexcept {
    handler_ = nullptr;
    return *this;
  }

  template <typename Fn,
            std::enable_if_t<!std::is_same_v<std::decay_t<Fn>, TerminalExecutionObserverT>, int> = 0>
  TerminalExecutionObserverT& operator=(Fn&& fn) {
    assign(std::forward<Fn>(fn));
    return *this;
  }

  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(handler_); }

  [[nodiscard]] TerminalExecutionAction operator()(const Execution& execution) const {
    if (!handler_) {
      return TerminalExecutionAction::Continue;
    }
    return handler_(execution);
  }

 private:
  template <typename Fn>
  static constexpr bool kAlwaysFalse = false;

  template <typename Fn>
  void assign(Fn&& fn) {
    using FnT = std::decay_t<Fn>;
    handler_ = [fn = FnT(std::forward<Fn>(fn))](const Execution& execution) mutable {
      if constexpr (std::is_invocable_r_v<TerminalExecutionAction, FnT&, const Execution&>) {
        return std::invoke(fn, execution);
      } else if constexpr (std::is_invocable_r_v<void, FnT&, const Execution&>) {
        std::invoke(fn, execution);
        return TerminalExecutionAction::Continue;
      } else if constexpr (std::is_invocable_r_v<TerminalExecutionAction, FnT&,
                                                 const model::ExplorationGraphT<ValueT>&>) {
        return std::invoke(fn, execution.graph);
      } else if constexpr (std::is_invocable_r_v<void, FnT&,
                                                 const model::ExplorationGraphT<ValueT>&>) {
        std::invoke(fn, execution.graph);
        return TerminalExecutionAction::Continue;
      } else {
        static_assert(kAlwaysFalse<FnT>,
                      "terminal execution observer must accept TerminalExecutionT or "
                      "ExplorationGraphT and return void or TerminalExecutionAction");
      }
    };
  }

  Handler handler_;
};

template <typename ValueT>
// Compatibility alias; prefer TerminalExecutionObserverT.
using ExecutionObserverT = TerminalExecutionObserverT<ValueT>;

template <typename ValueT>
struct DporConfigT {
  ProgramT<ValueT> program;
  // Bounds DPOR tree depth, not current graph size or implementation stack depth.
  std::size_t max_depth{1000};
  model::CommunicationModel communication_model{model::CommunicationModel::Async};
  TerminalExecutionObserverT<ValueT> on_terminal_execution{};
  ProgressObserver on_progress{};
  std::chrono::milliseconds progress_report_interval{std::chrono::seconds(1)};
  // Compatibility alias; prefer on_terminal_execution.
  TerminalExecutionObserverT<ValueT> on_execution{};
};

// Experimental options for verify_parallel(). A zero max_workers selects a
// hardware-based default; a zero max_queued_tasks derives a small queue budget
// from the resolved worker count.
struct ParallelVerifyOptions {
  std::size_t max_workers{0};
  std::size_t max_queued_tasks{0};
  // Uses the same DPOR tree-depth accounting as max_depth.
  std::size_t spawn_depth_cutoff{0};
  std::size_t min_fanout{2};
  // Workers read the shared stop flag only every sync_steps stop_requested()
  // calls. This reduces stop-check contention at the cost of weaker stop
  // semantics: workers may publish additional terminal executions after a
  // callback has requested stop. A zero value enables the strictest stop
  // checks, but the default uses a moderate batching value to reduce
  // synchronization overhead. Callbacks still run outside publication_mutex_,
  // so a terminal that already passed the stop check may still be published
  // before the stop request is committed.
  std::size_t sync_steps{512};
  // Flush thread-local terminal counters into shared progress counters after
  // this many local terminal publications. Smaller values improve live progress
  // accuracy; larger values reduce atomic-update overhead. A zero value uses a
  // small default.
  std::size_t progress_counter_flush_interval{1024};
  // When interval-throttled progress reporting is enabled, workers only poll
  // the clock and attempt to claim the next reporting window every
  // progress_poll_interval_steps internal progress checkpoints. Zero and one
  // both mean poll at every checkpoint; larger values reduce progress-polling
  // overhead at the cost of coarser timing for live snapshots.
  std::size_t progress_poll_interval_steps{64};
};

using DporConfig = DporConfigT<model::Value>;
using TerminalExecution = TerminalExecutionT<model::Value>;
using TerminalExecutionObserver = TerminalExecutionObserverT<model::Value>;
using ExecutionObserver = TerminalExecutionObserver;

namespace detail {

using EventId = typename model::ExplorationGraphT<model::Value>::EventId;

template <typename ValueT>
[[nodiscard]] inline TerminalExecutionAction notify_terminal_execution(
    const DporConfigT<ValueT>& config, const model::ExplorationGraphT<ValueT>& graph,
    const TerminalExecutionKind kind) {
  if (config.on_terminal_execution) {
    return config.on_terminal_execution(TerminalExecutionT<ValueT>{
        .graph = graph,
        .kind = kind,
    });
  }
  if (config.on_execution) {
    return config.on_execution(TerminalExecutionT<ValueT>{
        .graph = graph,
        .kind = kind,
    });
  }
  return TerminalExecutionAction::Continue;
}

[[nodiscard]] inline ProgressState progress_state_from_result_kind(
    const VerifyResultKind kind) noexcept {
  return kind == VerifyResultKind::Stopped ? ProgressState::Stopped : ProgressState::AllExplored;
}

template <typename ValueT>
inline void notify_progress(const DporConfigT<ValueT>& config, const ProgressSnapshot& snapshot) {
  if (config.on_progress) {
    config.on_progress(snapshot);
  }
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

template <typename ValueT>
[[nodiscard]] inline model::ConsistencyResult check_consistency(
    model::ExplorationGraphT<ValueT>& graph,
    const model::CommunicationModel communication_model) {
  model::ConsistencyCheckerT<ValueT> checker(communication_model);
  return checker.check(graph);
}

template <typename ValueT>
struct AllowMissingReadsForNonTargetT {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  EvId target_receive{model::ExplorationGraphT<ValueT>::kNoSource};

  [[nodiscard]] bool operator()(const EvId receive_id) const noexcept {
    return receive_id != target_receive;
  }
};

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

// Lightweight `(po ∪ rf)` view over a kept-event mask. Async revisit/tiebreaker
// checks use this to emulate `G|Previous` without materializing a child graph.
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

template <typename ValueT>
[[nodiscard]] inline model::ConsistencyResult check_consistency_allowing_missing_reads_except(
    model::ExplorationGraphT<ValueT>& graph,
    const typename model::ExplorationGraphT<ValueT>::EventId target_receive,
    const model::CommunicationModel communication_model) {
  return model::detail::check_exploration_graph(
      graph, communication_model, AllowMissingReadsForNonTargetT<ValueT>{target_receive});
}

template <typename ValueT>
[[nodiscard]] inline bool rf_rewrite_is_consistent(
    const model::ExplorationGraphT<ValueT>& graph,
    const typename model::ExplorationGraphT<ValueT>::EventId recv,
    const typename model::ExplorationGraphT<ValueT>::EventId send,
    const model::CommunicationModel communication_model,
    const bool allow_non_target_missing_reads = false) {
  if (communication_model == model::CommunicationModel::Async &&
      !allow_non_target_missing_reads) {
    return !rewiring_recv_creates_cycle(graph, recv, send);
  }

  const bool preserves_acyclicity =
      graph.is_known_acyclic() && !rewiring_recv_creates_cycle(graph, recv, send);
  auto rewritten = preserves_acyclicity ? graph.with_rf_preserving_known_acyclicity(recv, send)
                                        : graph.with_rf(recv, send);
  const auto consistency =
      allow_non_target_missing_reads
          ? check_consistency_allowing_missing_reads_except(rewritten, recv, communication_model)
          : check_consistency(rewritten, communication_model);
  return consistency.is_consistent();
}

// GETCONSTIEBREAKER: the tid-minimal send that remains consistent for recv to
// read from under the configured communication model.
template <typename ValueT>
[[nodiscard]] inline typename model::ExplorationGraphT<ValueT>::EventId get_cons_tiebreaker(
    const model::ExplorationGraphT<ValueT>& graph,
    typename model::ExplorationGraphT<ValueT>::EventId recv,
    const model::CommunicationModel communication_model = model::CommunicationModel::Async,
    const bool allow_non_target_missing_reads = false) {
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

  // Return the first candidate that remains consistent under the configured
  // communication model.
  for (const auto& candidate : candidates) {
    if (rf_rewrite_is_consistent(graph, recv, candidate.send_id, communication_model,
                                 allow_non_target_missing_reads)) {
      return candidate.send_id;
    }
  }

  throw std::logic_error("get_cons_tiebreaker invariant violated: no consistent source found");
}

template <typename ValueT>
[[nodiscard]] inline typename model::ExplorationGraphT<ValueT>::EventId
get_cons_tiebreaker_masked(const model::ExplorationGraphT<ValueT>& graph,
                           const std::vector<std::uint8_t>& keep_mask,
                           typename model::ExplorationGraphT<ValueT>::EventId recv,
                           const model::CommunicationModel communication_model =
                               model::CommunicationModel::Async) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  if (communication_model != model::CommunicationModel::Async) {
    auto restricted = model::detail::restrict_masked(graph, keep_mask);

    std::vector<EvId> new_to_old;
    new_to_old.reserve(restricted.event_count());
    EvId remapped_recv = model::ExplorationGraphT<ValueT>::kNoSource;
    for (const auto old_id : graph.insertion_order()) {
      if (old_id >= keep_mask.size() || keep_mask[old_id] == 0U) {
        continue;
      }
      if (old_id == recv) {
        remapped_recv = static_cast<EvId>(new_to_old.size());
      }
      new_to_old.push_back(old_id);
    }

    if (remapped_recv == model::ExplorationGraphT<ValueT>::kNoSource) {
      throw std::logic_error(
          "get_cons_tiebreaker invariant violated: event missing from keep mask");
    }

    const auto remapped_send = get_cons_tiebreaker(restricted, remapped_recv, communication_model,
                                                   true);
    if (remapped_send >= new_to_old.size()) {
      throw std::logic_error(
          "get_cons_tiebreaker invariant violated: remapped send missing from restricted graph");
    }
    return new_to_old[remapped_send];
  }

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
                                            typename model::ExplorationGraphT<ValueT>::EventId s,
                                            const model::CommunicationModel communication_model =
                                                model::CommunicationModel::Async) {
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

  const auto tiebreaker = get_cons_tiebreaker_masked(graph, previous, e, communication_model);
  return current_rf_original == tiebreaker;
}

enum class ExplorationTaskMode : std::uint8_t { Visit, VisitIfConsistent };

// DPOR tree depth is logical search-tree distance from the root:
// ordinary forward steps and backward revisits increment it by one, while
// blocked-reschedule preserves it because no new event is added.
template <typename ValueT>
struct ExplorationTask {
  model::ExplorationGraphT<ValueT> graph;
  std::size_t dpor_tree_depth{0};
  ExplorationTaskMode mode{ExplorationTaskMode::Visit};
};

template <typename ValueT, typename ExecutorT>
inline void visit_impl(const ProgramT<ValueT>& program, model::ExplorationGraphT<ValueT>& graph,
                       ExecutorT& executor, const DporConfigT<ValueT>& config,
                       std::size_t dpor_tree_depth,
                       const std::vector<model::ThreadId>& thread_ids);

template <typename ValueT, typename ExecutorT>
inline void visit_if_consistent_impl(const ProgramT<ValueT>& program,
                                     model::ExplorationGraphT<ValueT>& graph, ExecutorT& executor,
                                     const DporConfigT<ValueT>& config,
                                     std::size_t dpor_tree_depth,
                                     const std::vector<model::ThreadId>& thread_ids);

template <typename ValueT>
class SequentialExecutor {
 public:
  using Clock = std::chrono::steady_clock;

  SequentialExecutor(VerifyResult& result, const DporConfigT<ValueT>& config,
                     Clock::time_point start_time = Clock::now())
      : result_(result),
        config_(config),
        start_time_(start_time),
        next_progress_report_at_(start_time_) {
    if (config_.progress_report_interval > std::chrono::milliseconds::zero()) {
      next_progress_report_at_ = start_time_ + config_.progress_report_interval;
    }
  }

  [[nodiscard]] bool stop_requested() const noexcept {
    return result_.kind == VerifyResultKind::Stopped;
  }

  void maybe_report_progress() {
    if (!config_.on_progress) {
      return;
    }
    const auto now = Clock::now();
    if (config_.progress_report_interval > std::chrono::milliseconds::zero()) {
      if (now < next_progress_report_at_) {
        return;
      }
      next_progress_report_at_ = now + config_.progress_report_interval;
    }
    notify_progress(config_, make_progress_snapshot(ProgressState::Running, now));
  }

  [[nodiscard]] bool publish_depth_limit_execution(const model::ExplorationGraphT<ValueT>& graph) {
    ++result_.executions_explored;
    ++result_.depth_limit_executions_explored;
    if (notify_terminal_execution(config_, graph, TerminalExecutionKind::DepthLimit) ==
        TerminalExecutionAction::Stop) {
      request_stop();
    }
    return true;
  }

  [[nodiscard]] bool can_spawn(std::size_t /*dpor_tree_depth*/,
                               std::size_t /*fanout*/) const noexcept {
    return false;
  }

  [[nodiscard]] bool try_enqueue(ExplorationTask<ValueT>& /*task*/) const noexcept { return false; }

  [[nodiscard]] bool publish_full_execution(const model::ExplorationGraphT<ValueT>& graph) {
    ++result_.executions_explored;
    ++result_.full_executions_explored;
    if (notify_terminal_execution(config_, graph, TerminalExecutionKind::Full) ==
        TerminalExecutionAction::Stop) {
      request_stop();
    }
    return true;
  }

  [[nodiscard]] bool publish_error_execution(const model::ExplorationGraphT<ValueT>& graph,
                                             const model::ThreadId /*tid*/,
                                             const model::ErrorLabel& /*error*/) {
    ++result_.executions_explored;
    ++result_.error_executions_explored;
    if (notify_terminal_execution(config_, graph, TerminalExecutionKind::Error) ==
        TerminalExecutionAction::Stop) {
      request_stop();
    }
    return true;
  }

  void publish_final_progress() {
    if (!config_.on_progress) {
      return;
    }
    notify_progress(config_, make_progress_snapshot(progress_state_from_result_kind(result_.kind),
                                                   Clock::now()));
  }

 private:
  [[nodiscard]] ProgressSnapshot make_progress_snapshot(const ProgressState state,
                                                        const Clock::time_point now) const {
    ProgressSnapshot snapshot;
    snapshot.state = state;
    snapshot.elapsed = now - start_time_;
    snapshot.terminal_executions = result_.executions_explored;
    snapshot.full_executions = result_.full_executions_explored;
    snapshot.error_executions = result_.error_executions_explored;
    snapshot.depth_limit_executions = result_.depth_limit_executions_explored;
    snapshot.active_workers = state == ProgressState::Running ? 1 : 0;
    return snapshot;
  }

  void request_stop() noexcept { result_.kind = VerifyResultKind::Stopped; }

  VerifyResult& result_;
  const DporConfigT<ValueT>& config_;
  Clock::time_point start_time_;
  Clock::time_point next_progress_report_at_;
};

template <typename ValueT>
class ParallelExecutor {
 public:
  using Clock = std::chrono::steady_clock;
  using ProgressTick = typename Clock::duration::rep;

  ParallelExecutor(const DporConfigT<ValueT>& config, ParallelVerifyOptions options,
                   std::vector<model::ThreadId> thread_ids)
      : config_(config),
        options_(options),
        thread_ids_(std::move(thread_ids)),
        max_workers_(resolve_max_workers(options.max_workers)),
        max_queued_tasks_(resolve_max_queued_tasks(options.max_queued_tasks, max_workers_)),
        min_fanout_(std::max<std::size_t>(1, options.min_fanout)),
        sync_steps_(options.sync_steps),
        progress_counter_flush_interval_(
            resolve_progress_counter_flush_interval(options.progress_counter_flush_interval)),
        progress_poll_interval_steps_(options.progress_poll_interval_steps),
        start_time_(Clock::now()),
        next_progress_report_tick_(tick_for(start_time_)) {
    if (config_.progress_report_interval > std::chrono::milliseconds::zero()) {
      next_progress_report_tick_.store(tick_for(start_time_ + config_.progress_report_interval),
                                       std::memory_order_relaxed);
    }
  }

  [[nodiscard]] VerifyResult run() {
    {
      std::lock_guard lock(queue_mutex_);
      task_queue_.push(ExplorationTask<ValueT>{
          .graph = model::ExplorationGraphT<ValueT>{},
          .dpor_tree_depth = 0,
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
    result.full_executions_explored = full_executions_explored_.load(std::memory_order_relaxed);
    result.error_executions_explored = error_executions_explored_.load(std::memory_order_relaxed);
    result.depth_limit_executions_explored =
        depth_limit_executions_explored_.load(std::memory_order_relaxed);
    result.executions_explored = result.full_executions_explored +
                                 result.error_executions_explored +
                                 result.depth_limit_executions_explored;
    if (stop_requested_.load(std::memory_order_relaxed)) {
      result.kind = VerifyResultKind::Stopped;
    }
    publish_final_progress(result.kind);
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

  void maybe_report_progress() {
    if (!config_.on_progress) {
      return;
    }
    if (config_.progress_report_interval <= std::chrono::milliseconds::zero()) {
      publish_progress_snapshot(ProgressState::Running, Clock::now(), live_counts_exact());
      return;
    }

    if (progress_poll_interval_steps_ > 1) {
      auto& state = worker_state();
      if (++state.steps_since_progress_poll < progress_poll_interval_steps_) {
        return;
      }
      state.steps_since_progress_poll = 0;
    }

    const auto now = Clock::now();
    const auto now_tick = tick_for(now);
    auto due_tick = next_progress_report_tick_.load(std::memory_order_relaxed);
    if (now_tick < due_tick) {
      return;
    }

    const auto next_due_tick = tick_for(now + config_.progress_report_interval);
    while (true) {
      if (now_tick < due_tick) {
        return;
      }
      if (next_progress_report_tick_.compare_exchange_weak(
              due_tick, next_due_tick, std::memory_order_acq_rel, std::memory_order_relaxed)) {
        publish_progress_snapshot(ProgressState::Running, now, live_counts_exact());
        return;
      }
    }
  }

  [[nodiscard]] bool publish_depth_limit_execution(
      const model::ExplorationGraphT<ValueT>& graph) {
    if (sync_steps_ == 0) {
      std::lock_guard lock(publication_mutex_);
      if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
      }
      increment_local_terminal_counts(TerminalExecutionKind::DepthLimit);
    } else {
      if (stop_requested()) {
        return false;
      }
      increment_local_terminal_counts(TerminalExecutionKind::DepthLimit);
    }

    if (notify_terminal_execution(config_, graph, TerminalExecutionKind::DepthLimit) ==
        TerminalExecutionAction::Stop) {
      request_stop();
    }
    return true;
  }

  [[nodiscard]] bool can_spawn(const std::size_t child_dpor_tree_depth,
                               const std::size_t fanout) noexcept {
    if (max_workers_ <= 1 || stop_requested()) {
      return false;
    }
    if (fanout < min_fanout_) {
      return false;
    }
    if (options_.spawn_depth_cutoff != 0 &&
        child_dpor_tree_depth > options_.spawn_depth_cutoff) {
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

  [[nodiscard]] bool publish_full_execution(const model::ExplorationGraphT<ValueT>& graph) {
    if (sync_steps_ == 0) {
      // Serialise terminal-count updates with stop checks.
      std::lock_guard lock(publication_mutex_);
      if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
      }
      increment_local_terminal_counts(TerminalExecutionKind::Full);
    } else {
      if (stop_requested()) {
        return false;
      }
      increment_local_terminal_counts(TerminalExecutionKind::Full);
    }

    if (notify_terminal_execution(config_, graph, TerminalExecutionKind::Full) ==
        TerminalExecutionAction::Stop) {
      request_stop();
    }
    return true;
  }

  [[nodiscard]] bool publish_error_execution(const model::ExplorationGraphT<ValueT>& graph,
                                             const model::ThreadId /*tid*/,
                                             const model::ErrorLabel& /*error*/) {
    if (sync_steps_ == 0) {
      std::lock_guard lock(publication_mutex_);
      if (stop_requested_.load(std::memory_order_acquire)) {
        return false;
      }
      increment_local_terminal_counts(TerminalExecutionKind::Error);
    } else {
      if (stop_requested()) {
        return false;
      }
      increment_local_terminal_counts(TerminalExecutionKind::Error);
    }

    if (notify_terminal_execution(config_, graph, TerminalExecutionKind::Error) ==
        TerminalExecutionAction::Stop) {
      request_stop();
    }
    return true;
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

  [[nodiscard]] static std::size_t resolve_progress_counter_flush_interval(
      const std::size_t requested) {
    return requested == 0 ? 1024U : requested;
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
      visit_if_consistent_impl(config_.program, task.graph, *this, config_, task.dpor_tree_depth,
                               thread_ids_);
      return;
    }

    visit_impl(config_.program, task.graph, *this, config_, task.dpor_tree_depth, thread_ids_);
  }

  struct WorkerState {
    std::size_t local_full_executions{0};
    std::size_t local_error_executions{0};
    std::size_t local_depth_limit_executions{0};
    std::size_t pending_terminal_executions{0};
    std::size_t steps_since_sync{0};
    std::size_t steps_since_progress_poll{0};
    bool cached_stop{false};
  };

  WorkerState& worker_state() noexcept {
    thread_local WorkerState state;
    return state;
  }

  void increment_local_terminal_counts(const TerminalExecutionKind kind) {
    auto& state = worker_state();
    switch (kind) {
      case TerminalExecutionKind::Full:
        ++state.local_full_executions;
        break;
      case TerminalExecutionKind::Error:
        ++state.local_error_executions;
        break;
      case TerminalExecutionKind::DepthLimit:
        ++state.local_depth_limit_executions;
        break;
    }
    ++state.pending_terminal_executions;
    if (config_.on_progress && state.pending_terminal_executions >= progress_counter_flush_interval_) {
      flush_local_counts(state);
    }
  }

  void flush_worker_state() {
    flush_local_counts();
    auto& state = worker_state();
    state.steps_since_sync = 0;
    state.steps_since_progress_poll = 0;
    state.cached_stop = false;
  }

  void flush_local_counts() { flush_local_counts(worker_state()); }

  void flush_local_counts(WorkerState& state) {
    if (state.local_full_executions > 0) {
      full_executions_explored_.fetch_add(state.local_full_executions, std::memory_order_relaxed);
    }
    if (state.local_error_executions > 0) {
      error_executions_explored_.fetch_add(state.local_error_executions,
                                           std::memory_order_relaxed);
    }
    if (state.local_depth_limit_executions > 0) {
      depth_limit_executions_explored_.fetch_add(state.local_depth_limit_executions,
                                                 std::memory_order_relaxed);
    }
    state.local_full_executions = 0;
    state.local_error_executions = 0;
    state.local_depth_limit_executions = 0;
    state.pending_terminal_executions = 0;
  }

  [[nodiscard]] bool live_counts_exact() const noexcept {
    return progress_counter_flush_interval_ <= 1;
  }

  [[nodiscard]] static ProgressTick tick_for(const Clock::time_point time_point) noexcept {
    return time_point.time_since_epoch().count();
  }

  void publish_progress_snapshot(const ProgressState state, const Clock::time_point now,
                                 const bool counts_exact) {
    std::lock_guard lock(progress_callback_mutex_);
    flush_local_counts();
    notify_progress(config_, make_progress_snapshot(state, now, counts_exact));
  }

  void publish_final_progress(const VerifyResultKind result_kind) {
    if (!config_.on_progress) {
      return;
    }
    publish_progress_snapshot(progress_state_from_result_kind(result_kind), Clock::now(), true);
  }

  [[nodiscard]] ProgressSnapshot make_progress_snapshot(const ProgressState state,
                                                        const Clock::time_point now,
                                                        const bool counts_exact) const {
    ProgressSnapshot snapshot;
    snapshot.state = state;
    snapshot.elapsed = now - start_time_;
    snapshot.full_executions = full_executions_explored_.load(std::memory_order_relaxed);
    snapshot.error_executions = error_executions_explored_.load(std::memory_order_relaxed);
    snapshot.depth_limit_executions =
        depth_limit_executions_explored_.load(std::memory_order_relaxed);
    snapshot.terminal_executions = snapshot.full_executions + snapshot.error_executions +
                                   snapshot.depth_limit_executions;
    snapshot.max_workers = max_workers_;
    snapshot.max_queued_tasks = max_queued_tasks_;
    snapshot.counts_exact = counts_exact;
    {
      std::lock_guard lock(queue_mutex_);
      snapshot.active_workers = active_workers_;
      snapshot.queued_tasks = task_queue_.size();
    }
    return snapshot;
  }

  void request_stop() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    worker_state().cached_stop = true;
    queue_cv_.notify_all();
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
  std::size_t progress_counter_flush_interval_{1024};
  std::size_t progress_poll_interval_steps_{64};
  Clock::time_point start_time_;
  std::atomic<ProgressTick> next_progress_report_tick_{0};

  std::atomic<bool> stop_requested_{false};
  std::atomic<std::size_t> full_executions_explored_{0};
  std::atomic<std::size_t> error_executions_explored_{0};
  std::atomic<std::size_t> depth_limit_executions_explored_{0};

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::queue<ExplorationTask<ValueT>> task_queue_;
  std::size_t active_workers_{0};
  bool search_complete_{false};

  mutable std::mutex publication_mutex_;
  mutable std::mutex progress_callback_mutex_;
  std::exception_ptr first_exception_;
};

template <typename ValueT, typename ExecutorT>
[[nodiscard]] inline bool try_enqueue_owned_task(ExecutorT& executor,
                                                 model::ExplorationGraphT<ValueT>& graph,
                                                 const std::size_t dpor_tree_depth,
                                                 const ExplorationTaskMode mode) {
  ExplorationTask<ValueT> task{
      .graph = std::move(graph),
      .dpor_tree_depth = dpor_tree_depth,
      .mode = mode,
  };
  if (executor.try_enqueue(task)) {
    return true;
  }
  graph = std::move(task.graph);
  return false;
}

enum class ExplorationFrameKind : std::uint8_t {
  Enter,
  ExitLinearChild,
  ResumeNd,
  ResumeReceive,
  ResumeSendRevisits,
};

enum class BlockedReceiveRescheduleKind : std::uint8_t { None, Ready, StopRequested };

template <typename ValueT>
struct ExplorationFrame {
  using Checkpoint = typename model::ExplorationGraphT<ValueT>::Checkpoint;
  using EventId = typename model::ExplorationGraphT<ValueT>::EventId;

  ExplorationFrameKind kind{ExplorationFrameKind::Enter};
  std::size_t dpor_tree_depth{0};
  ExplorationTaskMode mode{ExplorationTaskMode::Visit};
  Checkpoint checkpoint{};
  std::size_t cursor{0};
  EventId event_id{model::ExplorationGraphT<ValueT>::kNoSource};
  bool flag{false};

  [[nodiscard]] static ExplorationFrame enter(const std::size_t frame_dpor_tree_depth,
                                              const ExplorationTaskMode frame_mode) {
    return ExplorationFrame{
        .kind = ExplorationFrameKind::Enter,
        .dpor_tree_depth = frame_dpor_tree_depth,
        .mode = frame_mode,
    };
  }
};

template <typename ValueT>
struct ExplorationContext {
  model::ExplorationGraphT<ValueT>* borrowed_graph{nullptr};
  std::optional<model::ExplorationGraphT<ValueT>> owned_graph{};
  std::vector<ExplorationFrame<ValueT>> frames;

  [[nodiscard]] model::ExplorationGraphT<ValueT>& graph() {
    if (owned_graph.has_value()) {
      return *owned_graph;
    }
    return *borrowed_graph;
  }

  [[nodiscard]] const model::ExplorationGraphT<ValueT>& graph() const {
    if (owned_graph.has_value()) {
      return *owned_graph;
    }
    return *borrowed_graph;
  }
};

template <typename ValueT>
struct BackwardRevisitChildResult {
  std::size_t next_receive_index{0};
  std::optional<model::ExplorationGraphT<ValueT>> child{};
};

template <typename ValueT>
struct BlockedReceiveRescheduleResult {
  BlockedReceiveRescheduleKind kind{BlockedReceiveRescheduleKind::None};
  std::optional<model::ExplorationGraphT<ValueT>> graph{};
};

template <typename ValueT>
[[nodiscard]] inline BackwardRevisitChildResult<ValueT> next_backward_revisit_child(
    const model::ExplorationGraphT<ValueT>& graph,
    const typename model::ExplorationGraphT<ValueT>::EventId send_id,
    const model::CommunicationModel communication_model,
    const std::size_t start_receive_index) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  const auto& send_evt = graph.event(send_id);
  const auto* send_label = model::as_send(send_evt);
  if (send_label == nullptr) {
    return BackwardRevisitChildResult<ValueT>{};
  }

  const auto receives = graph.receives_in_destination(send_id);

  for (std::size_t receive_index = start_receive_index; receive_index < receives.size();
       ++receive_index) {
    const auto recv_id = receives[receive_index];
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

    if (!revisit_condition(graph, recv_id, send_id, communication_model)) {
      continue;
    }
    bool all_pass = true;
    for (EvId ep = 0; ep < deleted.size(); ++ep) {
      if (deleted[ep] == 0U) {
        continue;
      }
      if (!revisit_condition(graph, ep, send_id, communication_model)) {
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
    return BackwardRevisitChildResult<ValueT>{
        .next_receive_index = receive_index + 1U,
        .child = std::move(restricted),
    };
  }

  return BackwardRevisitChildResult<ValueT>{
      .next_receive_index = receives.size(),
      .child = std::nullopt,
  };
}

template <typename ValueT, typename StopFn, typename EmitFn>
inline void for_each_backward_revisit_child(
    const model::ExplorationGraphT<ValueT>& graph,
    const typename model::ExplorationGraphT<ValueT>::EventId send_id,
    const model::CommunicationModel communication_model,
    StopFn&& should_stop,       // NOLINT(cppcoreguidelines-missing-std-forward)
    EmitFn&& emit_revisited) {  // NOLINT(cppcoreguidelines-missing-std-forward)
  std::size_t receive_index = 0;
  while (!should_stop()) {
    auto next =
        next_backward_revisit_child(graph, send_id, communication_model, receive_index);
    receive_index = next.next_receive_index;
    if (!next.child.has_value()) {
      return;
    }
    if (!emit_revisited(std::move(*next.child))) {
      return;
    }
  }
}

template <typename ValueT, typename ExecutorT>
[[nodiscard]] inline BlockedReceiveRescheduleResult<ValueT>
find_blocked_receive_reschedule_child(
    const ProgramT<ValueT>& program, const model::ExplorationGraphT<ValueT>& graph,
    ExecutorT& executor, const DporConfigT<ValueT>& config,
    const std::vector<model::ThreadId>& thread_ids) {
  constexpr auto kNoSource = model::ExplorationGraphT<ValueT>::kNoSource;

  for (const auto tid : thread_ids) {
    if (executor.stop_requested()) {
      return BlockedReceiveRescheduleResult<ValueT>{
          .kind = BlockedReceiveRescheduleKind::StopRequested,
      };
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

    return BlockedReceiveRescheduleResult<ValueT>{
        .kind = BlockedReceiveRescheduleKind::Ready,
        .graph = std::move(unblocked_graph),
    };
  }

  return BlockedReceiveRescheduleResult<ValueT>{};
}

template <typename ValueT, typename ExecutorT>
class DepthFirstExplorer {
 public:
  using Graph = model::ExplorationGraphT<ValueT>;
  using EventId = typename Graph::EventId;

  DepthFirstExplorer(const ProgramT<ValueT>& program, ExecutorT& executor,
                     const DporConfigT<ValueT>& config,
                     const std::vector<model::ThreadId>& thread_ids)
      : program_(program), executor_(executor), config_(config), thread_ids_(thread_ids) {}

  void run(Graph& graph, const std::size_t dpor_tree_depth, const ExplorationTaskMode mode) {
    contexts_.clear();
    contexts_.push_back(ExplorationContext<ValueT>{
        .borrowed_graph = &graph,
        .owned_graph = std::nullopt,
        .frames = {ExplorationFrame<ValueT>::enter(dpor_tree_depth, mode)},
    });
    run_loop();
  }

 private:
  [[nodiscard]] ExplorationContext<ValueT>& current_context() { return contexts_.back(); }

  [[nodiscard]] Graph& current_graph() { return current_context().graph(); }

  static void pop_context_if_empty(std::vector<ExplorationContext<ValueT>>& contexts) {
    if (!contexts.empty() && contexts.back().frames.empty()) {
      contexts.pop_back();
    }
  }

  void pop_top_frame() {
    auto& frames = current_context().frames;
    frames.pop_back();
    pop_context_if_empty(contexts_);
  }

  void rollback_and_pop_top_frame() {
    auto& context = current_context();
    const auto checkpoint = context.frames.back().checkpoint;
    context.graph().rollback(checkpoint);
    context.frames.pop_back();
    pop_context_if_empty(contexts_);
  }

  void push_owned_context(Graph graph, const std::size_t dpor_tree_depth,
                          const ExplorationTaskMode mode) {
    contexts_.push_back(ExplorationContext<ValueT>{
        .borrowed_graph = nullptr,
        .owned_graph = std::move(graph),
        .frames = {ExplorationFrame<ValueT>::enter(dpor_tree_depth, mode)},
    });
  }

  void unwind_one_step() {
    if (contexts_.empty()) {
      return;
    }
    if (current_context().frames.empty()) {
      contexts_.pop_back();
      return;
    }

    const auto kind = current_context().frames.back().kind;
    switch (kind) {
      case ExplorationFrameKind::Enter:
        pop_top_frame();
        return;
      case ExplorationFrameKind::ExitLinearChild:
      case ExplorationFrameKind::ResumeNd:
      case ExplorationFrameKind::ResumeReceive:
      case ExplorationFrameKind::ResumeSendRevisits:
        rollback_and_pop_top_frame();
        return;
    }
  }

  void run_loop() {
    try {
      while (!contexts_.empty()) {
        if (executor_.stop_requested()) {
          unwind_one_step();
          continue;
        }
        if (current_context().frames.empty()) {
          contexts_.pop_back();
          continue;
        }

        switch (current_context().frames.back().kind) {
          case ExplorationFrameKind::Enter:
            handle_enter_frame();
            break;
          case ExplorationFrameKind::ExitLinearChild:
            rollback_and_pop_top_frame();
            break;
          case ExplorationFrameKind::ResumeNd:
            handle_resume_nd_frame();
            break;
          case ExplorationFrameKind::ResumeReceive:
            handle_resume_receive_frame();
            break;
          case ExplorationFrameKind::ResumeSendRevisits:
            handle_resume_send_revisits_frame();
            break;
        }
      }
    } catch (...) {
      while (!contexts_.empty()) {
        unwind_one_step();
      }
      throw;
    }
  }

  void handle_enter_frame() {
    auto& context = current_context();
    auto& graph = context.graph();
    auto& frame = context.frames.back();

    if (frame.mode == ExplorationTaskMode::VisitIfConsistent) {
      if (executor_.stop_requested()) {
        pop_top_frame();
        return;
      }

      const auto consistency = check_consistency(graph, config_.communication_model);
      if (!consistency.is_consistent()) {
        pop_top_frame();
        return;
      }

      frame.mode = ExplorationTaskMode::Visit;
    }

    executor_.maybe_report_progress();
    if (executor_.stop_requested()) {
      return;
    }

    if (frame.dpor_tree_depth >= config_.max_depth) {
      static_cast<void>(executor_.publish_depth_limit_execution(graph));
      pop_top_frame();
      return;
    }

    const auto next = compute_next_event(program_, graph, thread_ids_);
    if (!next.has_value()) {
      auto reschedule =
          find_blocked_receive_reschedule_child(program_, graph, executor_, config_, thread_ids_);
      if (reschedule.kind == BlockedReceiveRescheduleKind::StopRequested) {
        return;
      }
      if (reschedule.kind == BlockedReceiveRescheduleKind::Ready) {
        const auto dpor_tree_depth = frame.dpor_tree_depth;
        pop_top_frame();
        push_owned_context(std::move(*reschedule.graph), dpor_tree_depth,
                           ExplorationTaskMode::Visit);
        return;
      }

      static_cast<void>(executor_.publish_full_execution(graph));
      pop_top_frame();
      return;
    }

    const auto [tid, label] = *next;

    if (const auto* error = std::get_if<model::ErrorLabel>(&label)) {
      const auto checkpoint = graph.checkpoint();
      static_cast<void>(graph.add_event(tid, label));
      static_cast<void>(executor_.publish_error_execution(graph, tid, *error));
      graph.rollback(checkpoint);
      pop_top_frame();
      return;
    }

    if (const auto* nd = std::get_if<model::NondeterministicChoiceLabelT<ValueT>>(&label)) {
      if (nd->choices.empty()) {
        const auto checkpoint = graph.checkpoint();
        static_cast<void>(graph.add_event(tid, label));
        frame.kind = ExplorationFrameKind::ExitLinearChild;
        frame.checkpoint = checkpoint;
        context.frames.push_back(
            ExplorationFrame<ValueT>::enter(frame.dpor_tree_depth + 1U,
                                            ExplorationTaskMode::Visit));
        return;
      }

      frame.kind = ExplorationFrameKind::ResumeNd;
      frame.checkpoint = graph.checkpoint();
      frame.cursor = 0;
      frame.event_id = Graph::kNoSource;
      frame.flag = false;
      return;
    }

    if (const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&label)) {
      frame.kind = ExplorationFrameKind::ResumeReceive;
      frame.checkpoint = graph.checkpoint();
      frame.cursor = 0;
      frame.event_id = Graph::kNoSource;
      frame.flag = recv->is_nonblocking();
      return;
    }

    if (std::holds_alternative<model::SendLabelT<ValueT>>(label)) {
      const auto checkpoint = graph.checkpoint();
      const auto send_id = graph.add_event(tid, label);
      frame.kind = ExplorationFrameKind::ResumeSendRevisits;
      frame.checkpoint = checkpoint;
      frame.cursor = 0;
      frame.event_id = send_id;
      frame.flag = executor_.can_spawn(frame.dpor_tree_depth + 1U, 2);
      return;
    }

    if (std::holds_alternative<model::BlockLabel>(label)) {
      const auto checkpoint = graph.checkpoint();
      static_cast<void>(graph.add_event(tid, label));
      frame.kind = ExplorationFrameKind::ExitLinearChild;
      frame.checkpoint = checkpoint;
      context.frames.push_back(
          ExplorationFrame<ValueT>::enter(frame.dpor_tree_depth + 1U,
                                          ExplorationTaskMode::Visit));
      return;
    }
  }

  void handle_resume_nd_frame() {
    auto& context = current_context();
    auto& graph = context.graph();
    auto& frame = context.frames.back();

    graph.rollback(frame.checkpoint);

    const auto next = compute_next_event(program_, graph, thread_ids_);
    if (!next.has_value()) {
      throw std::logic_error("ND resume frame lost its next event");
    }
    const auto [tid, label] = *next;
    const auto* nd = std::get_if<model::NondeterministicChoiceLabelT<ValueT>>(&label);
    if (nd == nullptr || nd->choices.empty()) {
      throw std::logic_error("ND resume frame expected a non-empty ND choice");
    }

    if (frame.cursor >= nd->choices.size()) {
      pop_top_frame();
      return;
    }

    auto nd_label = *nd;
    nd_label.value = nd->choices[frame.cursor];
    ++frame.cursor;
    const auto child_dpor_tree_depth = frame.dpor_tree_depth + 1U;
    static_cast<void>(graph.add_event(tid, model::EventLabelT<ValueT>{std::move(nd_label)}));
    context.frames.push_back(
        ExplorationFrame<ValueT>::enter(child_dpor_tree_depth, ExplorationTaskMode::Visit));
  }

  void handle_resume_receive_frame() {
    auto& context = current_context();
    auto& graph = context.graph();
    auto& frame = context.frames.back();

    graph.rollback(frame.checkpoint);

    const auto next = compute_next_event(program_, graph, thread_ids_);
    if (!next.has_value()) {
      throw std::logic_error("receive resume frame lost its next event");
    }
    const auto [tid, label] = *next;
    const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&label);
    if (recv == nullptr) {
      throw std::logic_error("receive resume frame expected a receive");
    }

    std::size_t compatible_index = 0;
    for (const auto send_id : graph.unread_send_event_ids()) {
      const auto* send = model::as_send(graph.event(send_id));
      if (send == nullptr || send->destination != tid || !recv->accepts(send->value)) {
        continue;
      }
      if (compatible_index == frame.cursor) {
        ++frame.cursor;
        const auto child_dpor_tree_depth = frame.dpor_tree_depth + 1U;
        const auto recv_id = graph.add_event(tid, label);
        graph.set_reads_from(recv_id, send_id);
        context.frames.push_back(
            ExplorationFrame<ValueT>::enter(child_dpor_tree_depth,
                                            ExplorationTaskMode::VisitIfConsistent));
        return;
      }
      ++compatible_index;
    }

    if (frame.flag && recv->is_nonblocking()) {
      frame.flag = false;
      const auto child_dpor_tree_depth = frame.dpor_tree_depth + 1U;
      const auto recv_id = graph.add_event(tid, label);
      graph.set_reads_from_bottom(recv_id);
      context.frames.push_back(
          ExplorationFrame<ValueT>::enter(child_dpor_tree_depth,
                                          ExplorationTaskMode::VisitIfConsistent));
      return;
    }

    pop_top_frame();
  }

  void handle_resume_send_revisits_frame() {
    auto& context = current_context();
    auto& graph = context.graph();
    auto& frame = context.frames.back();

    if (frame.event_id == Graph::kNoSource || !graph.is_valid_event_id(frame.event_id) ||
        !model::is_send(graph.event(frame.event_id))) {
      throw std::logic_error("send resume frame expected a live send event");
    }

    auto next = next_backward_revisit_child(graph, frame.event_id, config_.communication_model,
                                            frame.cursor);
    frame.cursor = next.next_receive_index;

    if (next.child.has_value()) {
      const auto child_dpor_tree_depth = frame.dpor_tree_depth + 1U;
      auto revisited = std::move(*next.child);
      if (frame.flag &&
          try_enqueue_owned_task<ValueT>(executor_, revisited, child_dpor_tree_depth,
                                         ExplorationTaskMode::VisitIfConsistent)) {
        return;
      }
      push_owned_context(std::move(revisited), child_dpor_tree_depth,
                         ExplorationTaskMode::VisitIfConsistent);
      return;
    }

    frame.kind = ExplorationFrameKind::ExitLinearChild;
    context.frames.push_back(
        ExplorationFrame<ValueT>::enter(frame.dpor_tree_depth + 1U,
                                        ExplorationTaskMode::Visit));
  }

  const ProgramT<ValueT>& program_;
  ExecutorT& executor_;
  const DporConfigT<ValueT>& config_;
  const std::vector<model::ThreadId>& thread_ids_;
  std::vector<ExplorationContext<ValueT>> contexts_;
};

template <typename ValueT, typename ExecutorT>
inline void visit_if_consistent_impl(const ProgramT<ValueT>& program,
                                     model::ExplorationGraphT<ValueT>& graph, ExecutorT& executor,
                                     const DporConfigT<ValueT>& config,
                                     const std::size_t dpor_tree_depth,
                                     const std::vector<model::ThreadId>& thread_ids) {
  DepthFirstExplorer<ValueT, ExecutorT> explorer(program, executor, config, thread_ids);
  explorer.run(graph, dpor_tree_depth, ExplorationTaskMode::VisitIfConsistent);
}

template <typename ValueT, typename ExecutorT>
inline void visit_impl(const ProgramT<ValueT>& program, model::ExplorationGraphT<ValueT>& graph,
                       ExecutorT& executor, const DporConfigT<ValueT>& config,
                       const std::size_t dpor_tree_depth,
                       const std::vector<model::ThreadId>& thread_ids) {
  DepthFirstExplorer<ValueT, ExecutorT> explorer(program, executor, config, thread_ids);
  explorer.run(graph, dpor_tree_depth, ExplorationTaskMode::Visit);
}

template <typename ValueT>
inline void visit_if_consistent(const ProgramT<ValueT>& program,
                                model::ExplorationGraphT<ValueT>& graph, VerifyResult& result,
                                const DporConfigT<ValueT>& config, std::size_t dpor_tree_depth,
                                const std::vector<model::ThreadId>& thread_ids) {
  SequentialExecutor<ValueT> executor(result, config);
  visit_if_consistent_impl(program, graph, executor, config, dpor_tree_depth, thread_ids);
}

template <typename ValueT>
inline void backward_revisit(const ProgramT<ValueT>& program,
                             const model::ExplorationGraphT<ValueT>& graph,
                             typename model::ExplorationGraphT<ValueT>::EventId send_id,
                             VerifyResult& result, const DporConfigT<ValueT>& config,
                             std::size_t dpor_tree_depth,
                             const std::vector<model::ThreadId>& thread_ids) {
  SequentialExecutor<ValueT> executor(result, config);
  DepthFirstExplorer<ValueT, SequentialExecutor<ValueT>> explorer(program, executor, config,
                                                                  thread_ids);
  for_each_backward_revisit_child(
      graph, send_id, config.communication_model, [&executor]() { return executor.stop_requested(); },
      [&](model::ExplorationGraphT<ValueT> revisited) {
        explorer.run(revisited, dpor_tree_depth, ExplorationTaskMode::VisitIfConsistent);
        return !executor.stop_requested();
      });
}

template <typename ValueT>
inline void visit(const ProgramT<ValueT>& program, model::ExplorationGraphT<ValueT>& graph,
                  VerifyResult& result, const DporConfigT<ValueT>& config,
                  std::size_t dpor_tree_depth,
                  const std::vector<model::ThreadId>& thread_ids) {
  SequentialExecutor<ValueT> executor(result, config);
  visit_impl(program, graph, executor, config, dpor_tree_depth, thread_ids);
}

}  // namespace detail

// VERIFY(P): entry point. Creates empty G₀, calls visit.
template <typename ValueT>
[[nodiscard]] inline VerifyResult verify(const DporConfigT<ValueT>& config) {
  VerifyResult result;
  model::ExplorationGraphT<ValueT> empty_graph;
  const auto thread_ids = detail::sorted_thread_ids(config.program);
  detail::SequentialExecutor<ValueT> executor(result, config);
  detail::visit_impl(config.program, empty_graph, executor, config, 0, thread_ids);
  executor.publish_final_progress();
  return result;
}

template <typename ValueT>
[[nodiscard]] inline VerifyResult verify_parallel(const DporConfigT<ValueT>& config,
                                                  ParallelVerifyOptions options = {}) {
  // Experimental. Callback order is unspecified across workers. With
  // sync_steps>0, additional terminal executions may still be published after a
  // callback has requested stop.
  const auto thread_ids = detail::sorted_thread_ids(config.program);
  detail::ParallelExecutor<ValueT> executor(config, options, thread_ids);
  return executor.run();
}

}  // namespace dpor::algo
