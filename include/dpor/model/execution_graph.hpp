#pragma once

#include "dpor/model/event.hpp"

#include <cstddef>
#include <optional>
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
  using ProgramOrderEdge = std::pair<EventId, EventId>;
  using ReadsFromSource = std::optional<EventId>;
  using ReadsFromRelation = std::unordered_map<EventId, ReadsFromSource>;

  [[nodiscard]] EventId add_event(Event event) {
    events_.push_back(std::move(event));
    return events_.size() - 1U;
  }

  void add_program_order_edge(EventId from, EventId to) {
    program_order_.emplace_back(from, to);
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

  [[nodiscard]] const std::vector<ProgramOrderEdge>& program_order() const noexcept {
    return program_order_;
  }

  [[nodiscard]] const ReadsFromRelation& reads_from() const noexcept {
    return reads_from_;
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
      if (source.has_value()) {
        consumed_send_ids.insert(*source);
      }
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
  std::vector<Event> events_{};
  std::vector<ProgramOrderEdge> program_order_{};
  ReadsFromRelation reads_from_{};
};

using ExecutionGraph = ExecutionGraphT<Value>;

}  // namespace dpor::model
