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
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::model {

template <typename ValueT>
class ExplorationGraphT;

template <typename EventIdT>
struct ReadsFromSourceT {
  std::optional<EventIdT> send{};

  [[nodiscard]] static ReadsFromSourceT from_send(EventIdT source) {
    return ReadsFromSourceT{
        .send = source,
    };
  }

  [[nodiscard]] static ReadsFromSourceT bottom() {
    return ReadsFromSourceT{};
  }

  [[nodiscard]] bool is_send() const noexcept {
    return send.has_value();
  }

  [[nodiscard]] bool is_bottom() const noexcept {
    return !send.has_value();
  }

  [[nodiscard]] EventIdT send_id() const {
    if (!send.has_value()) {
      throw std::logic_error("reads-from source is bottom");
    }
    return *send;
  }

  [[nodiscard]] bool operator==(EventIdT other) const noexcept {
    return send == other;
  }

  bool operator==(const ReadsFromSourceT&) const = default;
};

template <typename EventIdT>
[[nodiscard]] inline bool operator==(EventIdT lhs, const ReadsFromSourceT<EventIdT>& rhs) noexcept {
  return rhs == lhs;
}

template <typename EventIdT>
class ReadsFromRelationT {
 public:
  using ReadsFromSource = ReadsFromSourceT<EventIdT>;
  using Entry = std::pair<EventIdT, ReadsFromSource>;

  class const_iterator {
   public:
    struct arrow_proxy {
      Entry entry;

      [[nodiscard]] const Entry* operator->() const noexcept {
        return &entry;
      }
    };

    using iterator_category = std::input_iterator_tag;
    using value_type = Entry;
    using difference_type = std::ptrdiff_t;
    using pointer = arrow_proxy;
    using reference = value_type;

    const_iterator() = default;

    [[nodiscard]] reference operator*() const {
      return Entry{
          index_,
          *relation_->entries_.at(static_cast<std::size_t>(index_)),
      };
    }

    [[nodiscard]] pointer operator->() const {
      return arrow_proxy{**this};
    }

    const_iterator& operator++() {
      ++index_;
      advance_to_present();
      return *this;
    }

    const_iterator operator++(int) {
      auto copy = *this;
      ++(*this);
      return copy;
    }

    [[nodiscard]] bool operator==(const const_iterator& other) const noexcept {
      return relation_ == other.relation_ && index_ == other.index_;
    }

   private:
    friend class ReadsFromRelationT;

    const_iterator(const ReadsFromRelationT* relation, EventIdT index)
        : relation_(relation), index_(index) {
      advance_to_present();
    }

    void advance_to_present() {
      if (relation_ == nullptr) {
        return;
      }
      while (static_cast<std::size_t>(index_) < relation_->entries_.size() &&
             !relation_->entries_[static_cast<std::size_t>(index_)].has_value()) {
        ++index_;
      }
    }

    const ReadsFromRelationT* relation_{nullptr};
    EventIdT index_{0};
  };

  [[nodiscard]] const_iterator begin() const {
    return const_iterator(this, 0);
  }

  [[nodiscard]] const_iterator end() const {
    return const_iterator(this, static_cast<EventIdT>(entries_.size()));
  }

  [[nodiscard]] const_iterator find(EventIdT receive_id) const {
    const auto index = static_cast<std::size_t>(receive_id);
    if (index >= entries_.size() || !entries_[index].has_value()) {
      return end();
    }
    return const_iterator(this, receive_id);
  }

  [[nodiscard]] const ReadsFromSource& at(EventIdT receive_id) const {
    const auto index = static_cast<std::size_t>(receive_id);
    if (index >= entries_.size() || !entries_[index].has_value()) {
      throw std::out_of_range("reads-from entry not found");
    }
    return *entries_[index];
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return size_;
  }

  [[nodiscard]] bool empty() const noexcept {
    return size_ == 0;
  }

  void set(EventIdT receive_id, ReadsFromSource source) {
    const auto index = static_cast<std::size_t>(receive_id);
    if (index >= entries_.size()) {
      entries_.resize(index + 1);
    }
    if (!entries_[index].has_value()) {
      ++size_;
    }
    entries_[index] = std::move(source);
  }

 private:
  std::vector<std::optional<ReadsFromSource>> entries_{};
  std::size_t size_{0};
};

template <typename ValueT>
class ExecutionGraphT {
 public:
  using EventId = std::size_t;
  using Event = EventT<ValueT>;
  using ReadsFromSource = ReadsFromSourceT<EventId>;
  using ReadsFromRelation = ReadsFromRelationT<EventId>;

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
    ensure_thread_storage(thread);
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

  void set_reads_from_source(EventId receive_event_id, ReadsFromSource source) {
    reads_from_.set(receive_event_id, std::move(source));
  }

  void set_reads_from(EventId receive_event_id, EventId source_id) {
    set_reads_from_source(receive_event_id, ReadsFromSource::from_send(source_id));
  }

  void set_reads_from_bottom(EventId receive_event_id) {
    set_reads_from_source(receive_event_id, ReadsFromSource::bottom());
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

      if (source.is_bottom()) {
        continue;
      }

      const auto source_id = source.send_id();
      if (!is_valid_event_id(source_id)) {
        throw std::invalid_argument("reads-from relation source refers to an unknown send event id");
      }
      if (!is_send(events_[source_id])) {
        throw std::invalid_argument("reads-from relation source event is not a send");
      }

      relation.add_edge(source_id, receive_id);
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
      if (source.is_send()) {
        consumed_send_ids.insert(source.send_id());
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

  template <typename V>
  friend class ExplorationGraphT;

 private:
  void ensure_thread_storage(ThreadId thread) {
    const auto index = static_cast<std::size_t>(thread);
    if (index >= next_event_index_by_thread_.size()) {
      next_event_index_by_thread_.resize(index + 1, 0);
      used_event_indices_by_thread_.resize(index + 1);
    }
  }

  // Mutable access to events for in-place label modifications (e.g., ND value updates).
  [[nodiscard]] std::vector<Event>& events_mutable() noexcept {
    return events_;
  }

  [[nodiscard]] EventIndex next_event_index(ThreadId thread) {
    constexpr auto kMaxIndex = std::numeric_limits<EventIndex>::max();
    ensure_thread_storage(thread);
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
    std::vector<std::vector<EventId>> event_ids_by_thread;
    for (EventId id = 0; id < events_.size(); ++id) {
      const auto thread_index = static_cast<std::size_t>(events_[id].thread);
      if (thread_index >= event_ids_by_thread.size()) {
        event_ids_by_thread.resize(thread_index + 1);
      }
      event_ids_by_thread[thread_index].push_back(id);
    }

    std::vector<std::vector<NodeId>> thread_sequences;
    thread_sequences.reserve(event_ids_by_thread.size());

    for (auto& event_ids : event_ids_by_thread) {
      if (event_ids.empty()) {
        continue;
      }
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
  std::vector<EventIndex> next_event_index_by_thread_{};
  std::vector<std::unordered_set<EventIndex>> used_event_indices_by_thread_{};
};

using ExecutionGraph = ExecutionGraphT<Value>;

}  // namespace dpor::model
