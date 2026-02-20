#pragma once

// Execution-graph design rationale:
// - Keep event storage canonical (`events_`) and lightweight.
// - Store reads-from choices directly as receive -> send (`reads_from_`),
//   which matches exploration decisions and keeps the relation explicit.
// - Derive program order from per-thread `(thread, index)` metadata instead of
//   materializing all transitive PO edges; this keeps representation compact and
//   avoids duplicated state.
// - Export `po`/`rf` through the generic relation layer so downstream DPOR logic
//   can use common algebra (`compose`, closure, cycle checks) independently of
//   how each relation is represented internally.

#include "dpor/model/event.hpp"
#include "dpor/model/relation.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::model {

template <typename ValueT>
class ExecutionGraphT {
 public:
  using EventId = std::size_t;
  using Event = EventT<ValueT>;
  using ReadsFromSource = EventId;
  using ReadsFromRelation = std::unordered_map<EventId, ReadsFromSource>;

  // Normal insertion path: assign event index automatically per thread.
  [[nodiscard]] EventId add_event(ThreadId thread, EventLabelT<ValueT> label) {
    const auto index = next_event_index(thread);
    return add_event_with_index(thread, index, std::move(label));
  }

  // Replay/import path when event indices come from external traces.
  [[nodiscard]] EventId add_event_with_index(
      ThreadId thread,
      EventIndex index,
      EventLabelT<ValueT> label) {
    auto& used_indices = used_event_indices_by_thread_[thread];
    if (used_indices.find(index) != used_indices.end()) {
      throw std::invalid_argument("event index already used in this thread");
    }
    used_indices.insert(index);

    auto& next_index = next_event_index_by_thread_[thread];
    if (index >= next_index) {
      if (index == std::numeric_limits<EventIndex>::max()) {
        next_index = index;
      } else {
        next_index = static_cast<EventIndex>(index + 1);
      }
    }

    events_.push_back(Event{
        .thread = thread,
        .index = index,
        .label = std::move(label),
    });
    return events_.size() - 1U;
  }

  void set_reads_from(EventId receive_event_id, ReadsFromSource source) {
    reads_from_[receive_event_id] = source;
  }

  [[nodiscard]] bool is_valid_event_id(EventId event_id) const noexcept {
    return event_id < events_.size();
  }

  [[nodiscard]] const Event& event(EventId event_id) const {
    return events_.at(event_id);
  }

  [[nodiscard]] const std::vector<Event>& events() const noexcept {
    return events_;
  }

  [[nodiscard]] const ReadsFromRelation& reads_from() const noexcept {
    return reads_from_;
  }

  [[nodiscard]] ProgramOrderRelation po_relation() const {
    return ProgramOrderRelation(events_.size(), derive_thread_event_sequences());
  }

  // Builds an explicit send->receive relation view from stored receive->source
  // assignments.
  [[nodiscard]] ExplicitRelation rf_relation() const {
    ExplicitRelation relation(events_.size());

    for (const auto& [receive_id, source] : reads_from_) {
      if (!is_valid_event_id(receive_id)) {
        throw std::invalid_argument("reads-from relation refers to an unknown receive event id");
      }
      if (!is_receive(events_[receive_id])) {
        throw std::invalid_argument("reads-from relation target event is not a receive");
      }

      if (!is_valid_event_id(source)) {
        throw std::invalid_argument("reads-from relation source refers to an unknown send event id");
      }
      if (!is_send(events_[source])) {
        throw std::invalid_argument("reads-from relation source event is not a send");
      }

      relation.add_edge(source, receive_id);
    }

    return relation;
  }

  [[nodiscard]] std::vector<EventId> receive_event_ids() const {
    std::vector<EventId> ids;
    ids.reserve(events_.size());
    for (EventId i = 0; i < events_.size(); ++i) {
      if (is_receive(events_[i])) {
        ids.push_back(i);
      }
    }
    return ids;
  }

  [[nodiscard]] std::vector<EventId> send_event_ids() const {
    std::vector<EventId> ids;
    ids.reserve(events_.size());
    for (EventId i = 0; i < events_.size(); ++i) {
      if (is_send(events_[i])) {
        ids.push_back(i);
      }
    }
    return ids;
  }

  [[nodiscard]] std::vector<EventId> unread_send_event_ids() const {
    std::unordered_set<EventId> consumed_send_ids;
    consumed_send_ids.reserve(reads_from_.size());
    for (const auto& [_, source] : reads_from_) {
      consumed_send_ids.insert(source);
    }

    std::vector<EventId> unread;
    for (EventId send_id : send_event_ids()) {
      if (consumed_send_ids.find(send_id) == consumed_send_ids.end()) {
        unread.push_back(send_id);
      }
    }
    return unread;
  }

 private:
  [[nodiscard]] EventIndex next_event_index(ThreadId thread) {
    constexpr auto kMaxIndex = std::numeric_limits<EventIndex>::max();
    auto& next_index = next_event_index_by_thread_[thread];
    auto& used_indices = used_event_indices_by_thread_[thread];

    while (used_indices.find(next_index) != used_indices.end()) {
      if (next_index == kMaxIndex) {
        throw std::overflow_error("no available event index for thread");
      }
      ++next_index;
    }

    return next_index;
  }

  // Produces per-thread event sequences sorted by declared per-thread index.
  // These sequences are the canonical input for ProgramOrderRelation.
  [[nodiscard]] std::vector<std::vector<NodeId>> derive_thread_event_sequences() const {
    std::map<ThreadId, std::vector<EventId>> event_ids_by_thread;
    for (EventId id = 0; id < events_.size(); ++id) {
      event_ids_by_thread[events_[id].thread].push_back(id);
    }

    std::vector<std::vector<NodeId>> thread_sequences;
    thread_sequences.reserve(event_ids_by_thread.size());

    for (auto& [_, event_ids] : event_ids_by_thread) {
      std::sort(event_ids.begin(), event_ids.end(), [&](const EventId lhs, const EventId rhs) {
        const auto lhs_index = events_[lhs].index;
        const auto rhs_index = events_[rhs].index;
        if (lhs_index != rhs_index) {
          return lhs_index < rhs_index;
        }
        return lhs < rhs;
      });

      for (std::size_t i = 1; i < event_ids.size(); ++i) {
        const auto previous = event_ids[i - 1];
        const auto current = event_ids[i];
        if (events_[previous].index == events_[current].index) {
          throw std::invalid_argument(
              "two events in the same thread have the same event index; program order is ambiguous");
        }
      }

      thread_sequences.emplace_back(event_ids.begin(), event_ids.end());
    }

    return thread_sequences;
  }

  std::vector<Event> events_{};
  ReadsFromRelation reads_from_{};
  std::unordered_map<ThreadId, EventIndex> next_event_index_by_thread_{};
  std::unordered_map<ThreadId, std::unordered_set<EventIndex>> used_event_indices_by_thread_{};
};

using ExecutionGraph = ExecutionGraphT<Value>;

}  // namespace dpor::model
