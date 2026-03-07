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
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::algo {

enum class VerifyResultKind { AllExecutionsExplored, ErrorFound, DepthLimitReached };

struct VerifyResult {
  VerifyResultKind kind{VerifyResultKind::AllExecutionsExplored};
  std::string message{};
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

using DporConfig = DporConfigT<model::Value>;
using ExecutionObserver = ExecutionObserverT<model::Value>;

namespace detail {

using EventId = typename model::ExplorationGraphT<model::Value>::EventId;

template <typename ValueT>
[[nodiscard]] inline std::vector<model::ThreadId> sorted_thread_ids(
    const ProgramT<ValueT>& program) {
  program.threads.validate_compact_thread_ids();
  std::vector<model::ThreadId> thread_ids;
  thread_ids.reserve(program.threads.size());
  program.threads.for_each_assigned([&](const model::ThreadId tid, const auto&) {
    thread_ids.push_back(tid);
  });
  return thread_ids;
}

template <typename ValueT>
[[nodiscard]] inline bool has_compatible_unread_send(
    const model::ExplorationGraphT<ValueT>& graph,
    model::ThreadId tid,
    const model::ReceiveLabelT<ValueT>& receive) {
  for (const auto send_id : graph.unread_send_event_ids()) {
    const auto* send = model::as_send(graph.event(send_id));
    if (send != nullptr &&
        send->destination == tid &&
        receive.accepts(send->value)) {
      return true;
    }
  }
  return false;
}

// Compute the next event to add to the graph, following Algorithm 1's next_P(G).
// Iterates threads by ascending ThreadId, calls the thread function with the
// current trace, skips blocked/done threads, and turns unsatisfied blocking
// receives into internal Block events.
template <typename ValueT>
[[nodiscard]] inline std::optional<std::pair<model::ThreadId, model::EventLabelT<ValueT>>>
compute_next_event(
    const ProgramT<ValueT>& program,
    const model::ExplorationGraphT<ValueT>& graph,
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
      if (recv->is_blocking() &&
          !has_compatible_unread_send(graph, tid, *recv)) {
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
[[nodiscard]] inline std::unordered_set<typename model::ExplorationGraphT<ValueT>::EventId>
compute_previous_set(
    const model::ExplorationGraphT<ValueT>& graph,
    typename model::ExplorationGraphT<ValueT>::EventId e,
    typename model::ExplorationGraphT<ValueT>::EventId s) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  std::unordered_set<EvId> result;

  const auto n = graph.event_count();
  for (EvId ep = 0; ep < n; ++ep) {
    // e' ≤_G e: ep was inserted before or at e.
    if (graph.inserted_before_or_equal(ep, e)) {
      result.insert(ep);
      continue;
    }
    // ⟨e', s⟩ ∈ G.porf: ep reaches s through (po ∪ rf)+.
    if (graph.porf_contains(ep, s)) {
      result.insert(ep);
    }
  }

  return result;
}

// GETCONSTIEBREAKER: for async, the tid-minimal send that is consistent
// (no cycle when assigned as rf source for recv).
template <typename ValueT>
[[nodiscard]] inline typename model::ExplorationGraphT<ValueT>::EventId
get_cons_tiebreaker(
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
  if (send_label != nullptr &&
      send_label->destination == recv_evt.thread &&
      recv_label->accepts(send_label->value)) {
    candidates.push_back(Candidate{current_rf_source, send_evt.thread});
  }

  // Sort by sender thread ID (tid-minimal), then by event ID for stability.
  std::sort(candidates.begin(), candidates.end(),
      [](const Candidate& a, const Candidate& b) {
        if (a.sender_thread != b.sender_thread) {
          return a.sender_thread < b.sender_thread;
        }
        return a.send_id < b.send_id;
      });

  // Return the first candidate that doesn't create a cycle.
  for (const auto& candidate : candidates) {
    auto test_graph = graph.with_rf(recv, candidate.send_id);
    if (!test_graph.has_causal_cycle()) {
      return candidate.send_id;
    }
  }

  throw std::logic_error(
      "get_cons_tiebreaker invariant violated: no consistent source found");
}

// REVISITCONDITION(G, e, s):
// - ND → val(e) == min(S)
// - non-receive → no receive in Previous reads from e
// - receive → rf(e) == get_cons_tiebreaker(G|Previous, e)
template <typename ValueT>
[[nodiscard]] inline bool revisit_condition(
    const model::ExplorationGraphT<ValueT>& graph,
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
    for (const auto ep : previous) {
      if (model::is_receive(graph.event(ep))) {
        auto it = graph.reads_from().find(ep);
        if (it != graph.reads_from().end() &&
            it->second.is_send() &&
            it->second.send_id() == e) {
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
  auto restricted = graph.restrict(previous);

  // Build old-to-new ID mapping for events kept in the restricted graph.
  std::unordered_map<EvId, EvId> id_map;
  {
    EvId new_id = 0;
    for (const auto old_id : graph.insertion_order()) {
      if (previous.count(old_id) != 0U) {
        id_map[old_id] = new_id++;
      }
    }
  }

  // Remap e to its ID in the restricted graph.
  auto e_it = id_map.find(e);
  if (e_it == id_map.end()) {
    throw std::logic_error("revisit_condition invariant violated: event missing from Previous");
  }
  const auto remapped_e = e_it->second;

  // Remap current rf(e) to its ID in the restricted graph.
  // If rf(e) is not in Previous, the equality must fail (it cannot match any
  // tiebreaker source of G|Previous).
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
  auto rf_map_it = id_map.find(current_rf_original);
  if (rf_map_it == id_map.end()) {
    // This is a normal blocking-receive failure case, not an invariant break:
    // Algorithm 1 compares rf(e) against a tiebreaker computed on G|Previous,
    // so if the current source is not in Previous the equality cannot hold.
    return false;
  }
  const EvId remapped_rf = rf_map_it->second;

  const auto tiebreaker = get_cons_tiebreaker(restricted, remapped_e);
  return remapped_rf == tiebreaker;
}

// Forward declarations for mutual recursion.
template <typename ValueT>
inline void visit(
    const ProgramT<ValueT>& program,
    model::ExplorationGraphT<ValueT>& graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth,
    const std::vector<model::ThreadId>& thread_ids);

template <typename ValueT>
inline void visit_if_consistent(
    const ProgramT<ValueT>& program,
    model::ExplorationGraphT<ValueT>& graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth,
    const std::vector<model::ThreadId>& thread_ids);

// Must Example 4.1: before declaring completion, check whether a previously
// blocked receive can now be unblocked due to newly-added sends. If one exists,
// remove its Block event and continue exploration from the unblocked graph.
template <typename ValueT>
[[nodiscard]] inline bool reschedule_blocked_receive_if_enabled(
    const ProgramT<ValueT>& program,
    const model::ExplorationGraphT<ValueT>& graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth,
    const std::vector<model::ThreadId>& thread_ids) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;
  constexpr auto kNoSource = model::ExplorationGraphT<ValueT>::kNoSource;

  for (const auto tid : thread_ids) {
    const auto last_id = graph.last_event_id(tid);
    if (last_id == kNoSource || !model::is_block(graph.event(last_id))) {
      continue;
    }

    std::unordered_set<EvId> keep_set;
    keep_set.reserve(graph.event_count());
    for (EvId id = 0; id < graph.event_count(); ++id) {
      if (id != last_id) {
        keep_set.insert(id);
      }
    }
    auto unblocked_graph = graph.restrict(keep_set);

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
      throw std::logic_error(
          "blocked thread did not produce a receive after unblocking");
    }
    if (recv->is_nonblocking()) {
      throw std::logic_error(
          "blocked thread produced a non-blocking receive after unblocking");
    }
    if (!has_compatible_unread_send(unblocked_graph, tid, *recv)) {
      continue;
    }

    visit(program, unblocked_graph, result, config, depth, thread_ids);
    return true;
  }

  return false;
}

// Backward revisiting: lines 10-13 of Algorithm 1.
// For each receive in the destination thread of the new send:
// check compatibility, compute Deleted, check RevisitCondition, restrict, set rf, recurse.
template <typename ValueT>
inline void backward_revisit(
    const ProgramT<ValueT>& program,
    const model::ExplorationGraphT<ValueT>& graph,
    typename model::ExplorationGraphT<ValueT>::EventId send_id,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth,
    const std::vector<model::ThreadId>& thread_ids) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  if (result.kind == VerifyResultKind::ErrorFound) {
    return;
  }

  const auto& send_evt = graph.event(send_id);
  const auto* send_label = model::as_send(send_evt);
  if (send_label == nullptr) {
    return;
  }

  const auto receives = graph.receives_in_destination(send_id);

  for (const auto recv_id : receives) {
    if (result.kind == VerifyResultKind::ErrorFound) {
      return;
    }

    const auto& recv_evt = graph.event(recv_id);
    const auto* recv_label = model::as_receive(recv_evt);
    if (recv_label == nullptr) {
      continue;
    }

    // Check compatibility: the receive must accept the send's value.
    if (!recv_label->accepts(send_label->value)) {
      continue;
    }

    // Line 10 filter: skip if ⟨recv, send⟩ ∈ G.porf.
    if (graph.porf_contains(recv_id, send_id)) {
      continue;
    }

    // Line 11: Deleted = {e' ∈ G.E | r <_G e' ∧ ⟨e', send⟩ ∉ G.porf}
    // Note: send_id itself is never deleted — porf is irreflexive, but the
    // send trivially "reaches itself" in the paper's reflexive reading.
    std::unordered_set<EvId> deleted;
    for (EvId ep = 0; ep < graph.event_count(); ++ep) {
      if (ep == recv_id || ep == send_id) {
        continue;
      }
      if (graph.inserted_before_or_equal(ep, recv_id)) {
        continue;  // ep ≤_G r, so r does NOT strictly precede ep.
      }
      // ep is strictly after r in insertion order.
      if (!graph.porf_contains(ep, send_id)) {
        deleted.insert(ep);
      }
    }

    // Line 12: check revisit_condition for all e' ∈ Deleted ∪ {r}.
    if (!revisit_condition(graph, recv_id, send_id)) {
      continue;
    }
    bool all_pass = true;
    for (const auto ep : deleted) {
      if (!revisit_condition(graph, ep, send_id)) {
        all_pass = false;
        break;
      }
    }
    if (!all_pass) {
      continue;
    }

    // Line 13: keep_set = G.E \ Deleted.
    std::unordered_set<EvId> keep_set;
    for (EvId ep = 0; ep < graph.event_count(); ++ep) {
      if (deleted.count(ep) == 0U) {
        keep_set.insert(ep);
      }
    }

    auto restricted = graph.restrict(keep_set);

    // Map old IDs to new IDs in the restricted graph.
    std::vector<EvId> kept_in_order;
    kept_in_order.reserve(keep_set.size());
    for (const auto old_id : graph.insertion_order()) {
      if (keep_set.count(old_id) != 0U) {
        kept_in_order.push_back(old_id);
      }
    }

    EvId new_recv_id = model::ExplorationGraphT<ValueT>::kNoSource;
    EvId new_send_id = model::ExplorationGraphT<ValueT>::kNoSource;
    for (EvId new_id = 0; new_id < kept_in_order.size(); ++new_id) {
      if (kept_in_order[new_id] == recv_id) {
        new_recv_id = new_id;
      }
      if (kept_in_order[new_id] == send_id) {
        new_send_id = new_id;
      }
    }

    if (new_recv_id == model::ExplorationGraphT<ValueT>::kNoSource ||
        new_send_id == model::ExplorationGraphT<ValueT>::kNoSource) {
      continue;  // Should not happen if logic is correct.
    }

    auto revisited = restricted.with_rf(new_recv_id, new_send_id);
    visit_if_consistent(program, revisited, result, config, depth, thread_ids);
  }
}

// VISITIFCONSISTENT: check for causal cycle, then recurse.
template <typename ValueT>
inline void visit_if_consistent(
    const ProgramT<ValueT>& program,
    model::ExplorationGraphT<ValueT>& graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth,
    const std::vector<model::ThreadId>& thread_ids) {
  if (result.kind == VerifyResultKind::ErrorFound) {
    return;
  }

  model::AsyncConsistencyCheckerT<ValueT> checker;
  const auto consistency = checker.check(graph);
  if (!consistency.is_consistent()) {
    return;  // Inconsistent — prune.
  }

  visit(program, graph, result, config, depth, thread_ids);
}

// VISIT_P(G): main recursive procedure of Algorithm 1.
template <typename ValueT>
inline void visit(
    const ProgramT<ValueT>& program,
    model::ExplorationGraphT<ValueT>& graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth,
    const std::vector<model::ThreadId>& thread_ids) {
  using ScopedRollback = typename model::ExplorationGraphT<ValueT>::ScopedRollback;

  if (result.kind == VerifyResultKind::ErrorFound) {
    return;
  }

  if (depth >= config.max_depth) {
    if (result.kind == VerifyResultKind::AllExecutionsExplored) {
      result.kind = VerifyResultKind::DepthLimitReached;
    }
    return;
  }

  // Try to compute the next event.
  const auto next = compute_next_event(program, graph, thread_ids);

  if (!next.has_value()) {
    if (reschedule_blocked_receive_if_enabled(program, graph, result, config, depth, thread_ids)) {
      return;
    }
    // No more events: this is a complete execution.
    ++result.executions_explored;
    if (config.on_execution) {
      config.on_execution(graph);
    }
    return;
  }

  const auto& [tid, label] = *next;

  // Check for error events.
  if (std::holds_alternative<model::ErrorLabel>(label)) {
    ScopedRollback rollback(graph);
    static_cast<void>(graph.add_event(tid, label));
    ++result.executions_explored;
    result.kind = VerifyResultKind::ErrorFound;
    result.message = "error event reached in thread " + std::to_string(tid);
    if (config.on_execution) {
      config.on_execution(graph);
    }
    return;
  }

  // Handle ND choice: explore all choices.
  if (const auto* nd = std::get_if<model::NondeterministicChoiceLabelT<ValueT>>(&label)) {
    if (nd->choices.empty()) {
      // No choices specified: just use the value as-is.
      ScopedRollback rollback(graph);
      static_cast<void>(graph.add_event(tid, label));
      visit(program, graph, result, config, depth + 1, thread_ids);
      return;
    }

    for (const auto& choice : nd->choices) {
      if (result.kind == VerifyResultKind::ErrorFound) {
        return;
      }
      auto nd_label = *nd;
      nd_label.value = choice;
      {
        ScopedRollback rollback(graph);
        static_cast<void>(graph.add_event(tid, model::EventLabelT<ValueT>{nd_label}));
        visit(program, graph, result, config, depth + 1, thread_ids);
      }
    }
    return;
  }

  // Handle receive: try all compatible sends (forward exploration).
  if (const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&label)) {
    // Find all compatible unread sends.
    std::vector<typename model::ExplorationGraphT<ValueT>::EventId> compatible_sends;
    for (const auto send_id : graph.unread_send_event_ids()) {
      const auto* send = model::as_send(graph.event(send_id));
      if (send != nullptr &&
          send->destination == tid &&
          recv->accepts(send->value)) {
        compatible_sends.push_back(send_id);
      }
    }

    for (const auto send_id : compatible_sends) {
      if (result.kind == VerifyResultKind::ErrorFound) {
        return;
      }
      {
        ScopedRollback rollback(graph);
        const auto recv_id = graph.add_event(tid, label);
        graph.set_reads_from(recv_id, send_id);
        visit_if_consistent(program, graph, result, config, depth + 1, thread_ids);
      }
    }
    if (recv->is_nonblocking() && result.kind != VerifyResultKind::ErrorFound) {
      ScopedRollback rollback(graph);
      const auto recv_id = graph.add_event(tid, label);
      graph.set_reads_from_bottom(recv_id);
      visit_if_consistent(program, graph, result, config, depth + 1, thread_ids);
    }
    return;
  }

  // Handle send: add event, then do backward revisiting + forward continuation.
  if (const auto* send = std::get_if<model::SendLabelT<ValueT>>(&label)) {
    ScopedRollback rollback(graph);
    const auto send_id = graph.add_event(tid, label);

    // Backward revisit: try to reassign existing receives to read from this new send.
    backward_revisit(program, graph, send_id, result, config, depth + 1, thread_ids);

    // Forward continuation: continue exploration with the send added.
    if (result.kind != VerifyResultKind::ErrorFound) {
      visit(program, graph, result, config, depth + 1, thread_ids);
    }
    return;
  }

  // Handle block (internal receive-wait marker): add event and continue.
  if (std::holds_alternative<model::BlockLabel>(label)) {
    ScopedRollback rollback(graph);
    static_cast<void>(graph.add_event(tid, label));
    visit(program, graph, result, config, depth + 1, thread_ids);
    return;
  }
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

}  // namespace dpor::algo
