#pragma once

// ExplorationGraphT: insertion-ordered execution graph with DPOR operations.
//
// Wraps ExecutionGraphT<V> and adds:
// - Insertion order tracking (needed for <=_G and backward revisiting)
// - restrict(keep_set): returns a new graph with only specified events
// - with_rf(recv, send): returns a copy with rf assignment changed
// - with_nd_value(nd_event, value): returns a copy with ND choice value set
// - thread_trace(tid): extracts value sequence for a thread
// - porf_contains(from, to): checks (po ∪ rf)+ reachability
// - receives_in_destination(send_id): receives in the send's destination thread
// - has_causal_cycle(): lightweight cycle check
//
// Graph operations (restrict, with_rf, with_nd_value) return copies.
// This matches the recursive branching nature of Algorithm 1.

#include "dpor/model/event.hpp"
#include "dpor/model/execution_graph.hpp"
#include "dpor/model/relation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::model {

template <typename ValueT>
class ExplorationGraphT {
 public:
  using EventId = typename ExecutionGraphT<ValueT>::EventId;
  using Event = EventT<ValueT>;

  static constexpr EventId kNoSource = std::numeric_limits<EventId>::max();

  ExplorationGraphT() = default;

  // Add an event, tracking insertion order.
  [[nodiscard]] EventId add_event(ThreadId thread, EventLabelT<ValueT> label) {
    const auto id = graph_.add_event(thread, std::move(label));
    insertion_order_.push_back(id);
    insertion_position_[id] = insertion_order_.size() - 1U;
    return id;
  }

  void set_reads_from(EventId receive_id, EventId source_id) {
    graph_.set_reads_from(receive_id, source_id);
  }

  [[nodiscard]] const Event& event(EventId id) const {
    return graph_.event(id);
  }

  [[nodiscard]] const std::vector<Event>& events() const noexcept {
    return graph_.events();
  }

  [[nodiscard]] std::size_t event_count() const noexcept {
    return graph_.events().size();
  }

  [[nodiscard]] bool is_valid_event_id(EventId id) const noexcept {
    return graph_.is_valid_event_id(id);
  }

  [[nodiscard]] const typename ExecutionGraphT<ValueT>::ReadsFromRelation& reads_from() const noexcept {
    return graph_.reads_from();
  }

  [[nodiscard]] const std::vector<EventId>& insertion_order() const noexcept {
    return insertion_order_;
  }

  // Returns true if event a was inserted before event b (<=_G).
  [[nodiscard]] bool inserted_before_or_equal(EventId a, EventId b) const {
    const auto it_a = insertion_position_.find(a);
    const auto it_b = insertion_position_.find(b);
    if (it_a == insertion_position_.end() || it_b == insertion_position_.end()) {
      throw std::out_of_range("event id not found in insertion order");
    }
    return it_a->second <= it_b->second;
  }

  [[nodiscard]] ProgramOrderRelation po_relation() const {
    return graph_.po_relation();
  }

  [[nodiscard]] ExplicitRelation rf_relation() const {
    return graph_.rf_relation();
  }

  [[nodiscard]] std::vector<EventId> receive_event_ids() const {
    return graph_.receive_event_ids();
  }

  [[nodiscard]] std::vector<EventId> send_event_ids() const {
    return graph_.send_event_ids();
  }

  [[nodiscard]] std::vector<EventId> unread_send_event_ids() const {
    return graph_.unread_send_event_ids();
  }

  // Returns true if the thread's last event is a BlockLabel or ErrorLabel,
  // meaning the thread cannot produce further events.
  [[nodiscard]] bool thread_is_terminated(ThreadId tid) const {
    // Find the last event (by event index) for this thread.
    EventId last_id = kNoSource;
    EventIndex last_index = 0;
    for (EventId id = 0; id < event_count(); ++id) {
      const auto& evt = event(id);
      if (evt.thread == tid) {
        if (last_id == kNoSource || evt.index > last_index) {
          last_id = id;
          last_index = evt.index;
        }
      }
    }
    if (last_id == kNoSource) {
      return false;  // No events for this thread yet.
    }
    const auto& last_evt = event(last_id);
    return is_block(last_evt) || is_error(last_evt);
  }

  // Count events belonging to the given thread.
  [[nodiscard]] std::size_t thread_event_count(ThreadId tid) const {
    std::size_t count = 0;
    for (EventId id = 0; id < event_count(); ++id) {
      if (event(id).thread == tid) {
        ++count;
      }
    }
    return count;
  }

  // Extract the value sequence visible to a thread: values from receives (via rf)
  // and ND choices, in program order.
  [[nodiscard]] std::vector<ValueT> thread_trace(ThreadId tid) const {
    // Gather events for this thread, sorted by event index (program order).
    std::vector<std::pair<EventIndex, EventId>> thread_events;
    for (EventId id = 0; id < event_count(); ++id) {
      const auto& evt = event(id);
      if (evt.thread == tid) {
        thread_events.emplace_back(evt.index, id);
      }
    }
    std::sort(thread_events.begin(), thread_events.end());

    std::vector<ValueT> trace;
    const auto& rf = reads_from();

    for (const auto& [_, id] : thread_events) {
      const auto& evt = event(id);
      if (const auto* recv = as_receive(evt)) {
        auto it = rf.find(id);
        if (it != rf.end()) {
          const auto& source_evt = event(it->second);
          if (const auto* send = as_send(source_evt)) {
            trace.push_back(send->value);
          }
        }
      } else if (const auto* nd = as_nondeterministic_choice(evt)) {
        trace.push_back(nd->value);
      }
    }

    return trace;
  }

  // Returns a new graph containing only the events in keep_set.
  // IDs are remapped to [0, keep_set.size()), preserving relative insertion order.
  [[nodiscard]] ExplorationGraphT restrict(const std::unordered_set<EventId>& keep_set) const {
    // Build a list of kept events in insertion order.
    std::vector<EventId> kept_ids;
    kept_ids.reserve(keep_set.size());
    for (const auto old_id : insertion_order_) {
      if (keep_set.count(old_id) != 0U) {
        kept_ids.push_back(old_id);
      }
    }

    // Build old->new ID mapping.
    std::unordered_map<EventId, EventId> id_map;
    id_map.reserve(kept_ids.size());
    for (EventId new_id = 0; new_id < kept_ids.size(); ++new_id) {
      id_map[kept_ids[new_id]] = new_id;
    }

    ExplorationGraphT result;

    // Re-insert events in insertion order with remapped IDs.
    // We need to track per-thread index counters to assign fresh indices.
    std::unordered_map<ThreadId, EventIndex> thread_index_counter;
    for (const auto old_id : kept_ids) {
      const auto& evt = event(old_id);
      auto label = evt.label;

      // Remap send destination: destinations are thread IDs, not event IDs,
      // so they stay unchanged.
      const auto new_id = result.add_event(evt.thread, std::move(label));
      static_cast<void>(new_id);
    }

    // Remap reads-from edges.
    const auto& rf = reads_from();
    for (const auto& [recv_id, source_id] : rf) {
      auto recv_it = id_map.find(recv_id);
      auto source_it = id_map.find(source_id);
      if (recv_it != id_map.end() && source_it != id_map.end()) {
        result.set_reads_from(recv_it->second, source_it->second);
      }
    }

    return result;
  }

  // Returns a copy with the rf assignment for recv changed to send.
  [[nodiscard]] ExplorationGraphT with_rf(EventId recv, EventId send) const {
    auto copy = *this;
    copy.graph_.set_reads_from(recv, send);
    return copy;
  }

  // Returns a copy with the ND choice value for nd_event changed.
  [[nodiscard]] ExplorationGraphT with_nd_value(EventId nd_event, ValueT value) const {
    auto copy = *this;
    auto& events = copy.graph_.events_mutable();
    auto& evt = events.at(nd_event);
    auto* nd = std::get_if<NondeterministicChoiceLabelT<ValueT>>(&evt.label);
    if (nd == nullptr) {
      throw std::invalid_argument("event is not a nondeterministic choice");
    }
    nd->value = std::move(value);
    return copy;
  }

  // Check if (from, to) is in (po ∪ rf)+.
  [[nodiscard]] bool porf_contains(EventId from, EventId to) const {
    const auto po = po_relation();
    const auto rf = rf_relation();
    const auto porf = relation_union(po, rf);
    const auto porf_plus = transitive_closure(porf);
    return porf_plus.contains(from, to);
  }

  // Returns receive event IDs in the destination thread of the given send.
  [[nodiscard]] std::vector<EventId> receives_in_destination(EventId send_id) const {
    const auto* send = as_send(event(send_id));
    if (send == nullptr) {
      throw std::invalid_argument("event is not a send");
    }
    const auto dest_thread = send->destination;

    std::vector<EventId> result;
    for (EventId id = 0; id < event_count(); ++id) {
      const auto& evt = event(id);
      if (evt.thread == dest_thread && is_receive(evt)) {
        result.push_back(id);
      }
    }
    return result;
  }

  // Lightweight causal cycle check on (po ∪ rf).
  [[nodiscard]] bool has_causal_cycle() const {
    const auto n = event_count();
    if (n == 0) {
      return false;
    }

    // Build adjacency: po edges + rf edges.
    std::vector<std::vector<EventId>> successors(n);

    const auto po = po_relation();
    for (EventId from = 0; from < n; ++from) {
      po.for_each_successor(from, [&](const NodeId to) {
        successors[from].push_back(to);
      });
    }

    for (const auto& [recv_id, source_id] : reads_from()) {
      if (source_id < n && recv_id < n) {
        successors[source_id].push_back(recv_id);
      }
    }

    // DFS-based cycle detection.
    // 0 = unvisited, 1 = visiting (on stack), 2 = visited.
    std::vector<std::uint8_t> visit_state(n, 0U);

    const auto dfs = [&](const auto& self, const EventId node) -> bool {
      visit_state[node] = 1U;
      for (const EventId successor : successors[node]) {
        const auto state = visit_state[successor];
        if (state == 1U) {
          return true;
        }
        if (state == 0U && self(self, successor)) {
          return true;
        }
      }
      visit_state[node] = 2U;
      return false;
    };

    for (EventId id = 0; id < n; ++id) {
      if (visit_state[id] == 0U && dfs(dfs, id)) {
        return true;
      }
    }

    return false;
  }

  // Access to underlying execution graph (for consistency checker etc.)
  [[nodiscard]] const ExecutionGraphT<ValueT>& execution_graph() const noexcept {
    return graph_;
  }

 private:
  ExecutionGraphT<ValueT> graph_{};
  std::vector<EventId> insertion_order_{};
  std::unordered_map<EventId, std::size_t> insertion_position_{};
};

using ExplorationGraph = ExplorationGraphT<Value>;

}  // namespace dpor::model
