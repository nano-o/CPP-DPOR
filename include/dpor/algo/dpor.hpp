#pragma once

// DPOR exploration engine — Algorithm 1 from Enea et al., 2024.
//
// Given a program (collection of thread functions), explores all consistent
// execution graphs in a complete and optimal manner for the async communication
// model. Implements backward revisiting.
//
// All functions are header-only and templated on ValueT.

#include "dpor/algo/program.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::algo {

enum class VerifyResultKind { AllExecutionsExplored, ErrorFound };

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

// Compute the next event to add to the graph, following Algorithm 1's next_P(G).
// Iterates threads by ascending ThreadId, calls the thread function with the
// current trace, skips blocked/done threads, skips receives with no compatible sends.
template <typename ValueT>
[[nodiscard]] inline std::optional<std::pair<model::ThreadId, model::EventLabelT<ValueT>>>
compute_next_event(
    const ProgramT<ValueT>& program,
    const model::ExplorationGraphT<ValueT>& graph) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;

  // Collect thread IDs and sort them for deterministic iteration.
  std::vector<model::ThreadId> thread_ids;
  thread_ids.reserve(program.threads.size());
  for (const auto& [tid, _] : program.threads) {
    thread_ids.push_back(tid);
  }
  std::sort(thread_ids.begin(), thread_ids.end());

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

    // If it's a receive, check if there's at least one compatible unread send.
    if (const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&label)) {
      bool has_compatible_send = false;
      for (const auto send_id : graph.unread_send_event_ids()) {
        const auto* send = model::as_send(graph.event(send_id));
        if (send != nullptr &&
            send->destination == tid &&
            recv->accepts(send->value)) {
          has_compatible_send = true;
          break;
        }
      }
      if (!has_compatible_send) {
        continue;  // Blocked — no compatible send available.
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
  constexpr auto kNoSource = model::ExplorationGraphT<ValueT>::kNoSource;

  const auto& recv_evt = graph.event(recv);
  const auto* recv_label = model::as_receive(recv_evt);
  if (recv_label == nullptr) {
    return kNoSource;
  }

  // Find all compatible unread sends targeting recv's thread.
  struct Candidate {
    EvId send_id;
    model::ThreadId sender_thread;
  };

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

  // Also consider already-consumed sends (any send targeting this thread and compatible).
  // Actually, per the paper, we look at all sends in the graph, not just unread.
  // The tiebreaker is about which send *could* be assigned as rf source.
  candidates.clear();
  for (EvId id = 0; id < graph.event_count(); ++id) {
    const auto& evt = graph.event(id);
    const auto* send_label = model::as_send(evt);
    if (send_label == nullptr) {
      continue;
    }
    if (send_label->destination != recv_evt.thread) {
      continue;
    }
    if (!recv_label->accepts(send_label->value)) {
      continue;
    }
    candidates.push_back(Candidate{id, evt.thread});
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

  return kNoSource;
}

// REVISITCONDITION(G, e, s):
// - ND → val(e) == choices[0]
// - non-receive → no receive in Previous reads from e
// - receive → rf(e) == get_cons_tiebreaker(G, e)
template <typename ValueT>
[[nodiscard]] inline bool revisit_condition(
    const model::ExplorationGraphT<ValueT>& graph,
    typename model::ExplorationGraphT<ValueT>::EventId e,
    typename model::ExplorationGraphT<ValueT>::EventId s) {
  using EvId = typename model::ExplorationGraphT<ValueT>::EventId;
  constexpr auto kNoSource = model::ExplorationGraphT<ValueT>::kNoSource;

  const auto& evt = graph.event(e);

  // ND choice: val(e) == choices[0]
  if (const auto* nd = model::as_nondeterministic_choice(evt)) {
    if (nd->choices.empty()) {
      return true;
    }
    return nd->value == nd->choices[0];
  }

  // Non-receive event (send, block, error): check that no receive in Previous
  // reads from e.
  if (!model::is_receive(evt)) {
    const auto previous = compute_previous_set(graph, e, s);
    for (const auto ep : previous) {
      if (model::is_receive(graph.event(ep))) {
        auto it = graph.reads_from().find(ep);
        if (it != graph.reads_from().end() && it->second == e) {
          return false;  // A receive in Previous reads from e.
        }
      }
    }
    return true;
  }

  // Receive: rf(e) == get_cons_tiebreaker(G, e)
  auto it = graph.reads_from().find(e);
  const auto current_rf = (it != graph.reads_from().end()) ? it->second : kNoSource;
  const auto tiebreaker = get_cons_tiebreaker(graph, e);
  return current_rf == tiebreaker;
}

// Forward declarations for mutual recursion.
template <typename ValueT>
inline void visit(
    const ProgramT<ValueT>& program,
    model::ExplorationGraphT<ValueT> graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth);

template <typename ValueT>
inline void visit_if_consistent(
    const ProgramT<ValueT>& program,
    model::ExplorationGraphT<ValueT> graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth);

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
    std::size_t depth) {
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

    // Check revisit condition.
    if (!revisit_condition(graph, recv_id, send_id)) {
      continue;
    }

    // Compute the Previous set and use it as the keep set for restrict,
    // plus add the send itself.
    auto keep_set = compute_previous_set(graph, recv_id, send_id);
    keep_set.insert(send_id);
    // Also keep recv_id itself.
    keep_set.insert(recv_id);

    auto restricted = graph.restrict(keep_set);

    // In the restricted graph, find the remapped IDs for recv and send.
    // We need to find them by matching. Build a mapping from old events to new positions.
    // The restrict function keeps events in insertion order; we can find our events by
    // iterating the restricted graph and matching thread + index.

    // Actually, let's build the mapping more carefully.
    // Events in the restricted graph correspond to the kept IDs in insertion order.
    std::vector<EvId> kept_in_order;
    kept_in_order.reserve(keep_set.size());
    for (const auto old_id : graph.insertion_order()) {
      if (keep_set.count(old_id) != 0U) {
        kept_in_order.push_back(old_id);
      }
    }

    // Find the new IDs for recv and send.
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
    visit_if_consistent(program, std::move(revisited), result, config, depth);
  }
}

// VISITIFCONSISTENT: check for causal cycle, then recurse.
template <typename ValueT>
inline void visit_if_consistent(
    const ProgramT<ValueT>& program,
    model::ExplorationGraphT<ValueT> graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth) {
  if (result.kind == VerifyResultKind::ErrorFound) {
    return;
  }

  if (graph.has_causal_cycle()) {
    return;  // Prune inconsistent graphs.
  }

  visit(program, std::move(graph), result, config, depth);
}

// VISIT_P(G): main recursive procedure of Algorithm 1.
template <typename ValueT>
inline void visit(
    const ProgramT<ValueT>& program,
    model::ExplorationGraphT<ValueT> graph,
    VerifyResult& result,
    const DporConfigT<ValueT>& config,
    std::size_t depth) {
  if (result.kind == VerifyResultKind::ErrorFound) {
    return;
  }

  if (depth >= config.max_depth) {
    return;
  }

  // Try to compute the next event.
  const auto next = compute_next_event(program, graph);

  if (!next.has_value()) {
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
    const auto event_id = graph.add_event(tid, label);
    static_cast<void>(event_id);
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
      auto new_graph = graph;
      static_cast<void>(new_graph.add_event(tid, label));
      visit(program, std::move(new_graph), result, config, depth + 1);
      return;
    }

    for (const auto& choice : nd->choices) {
      if (result.kind == VerifyResultKind::ErrorFound) {
        return;
      }
      auto nd_label = *nd;
      nd_label.value = choice;
      auto new_graph = graph;
      static_cast<void>(new_graph.add_event(tid, model::EventLabelT<ValueT>{nd_label}));
      visit(program, std::move(new_graph), result, config, depth + 1);
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
      auto new_graph = graph;
      const auto recv_id = new_graph.add_event(tid, label);
      new_graph.set_reads_from(recv_id, send_id);
      visit_if_consistent(program, std::move(new_graph), result, config, depth + 1);
    }
    return;
  }

  // Handle send: add event, then do backward revisiting + forward continuation.
  if (const auto* send = std::get_if<model::SendLabelT<ValueT>>(&label)) {
    auto new_graph = graph;
    const auto send_id = new_graph.add_event(tid, label);

    // Backward revisit: try to reassign existing receives to read from this new send.
    backward_revisit(program, new_graph, send_id, result, config, depth + 1);

    // Forward continuation: continue exploration with the send added.
    if (result.kind != VerifyResultKind::ErrorFound) {
      visit(program, std::move(new_graph), result, config, depth + 1);
    }
    return;
  }

  // Handle block: add event and continue.
  if (std::holds_alternative<model::BlockLabel>(label)) {
    auto new_graph = graph;
    static_cast<void>(new_graph.add_event(tid, label));
    visit(program, std::move(new_graph), result, config, depth + 1);
    return;
  }
}

}  // namespace detail

// VERIFY(P): entry point. Creates empty G₀, calls visit.
template <typename ValueT>
[[nodiscard]] inline VerifyResult verify(const DporConfigT<ValueT>& config) {
  VerifyResult result;
  model::ExplorationGraphT<ValueT> empty_graph;
  detail::visit(config.program, std::move(empty_graph), result, config, 0);
  return result;
}

}  // namespace dpor::algo
