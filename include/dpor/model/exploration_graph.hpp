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
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::model {

struct PorfCache {
  std::vector<std::vector<std::size_t>> clocks;
  std::vector<std::size_t> position_in_thread;
  std::unordered_map<ThreadId, std::size_t> thread_clock_index;
  std::size_t num_threads{0};
  bool has_cycle{false};
};

template <typename ValueT>
class ExplorationGraphT {
 public:
  using EventId = typename ExecutionGraphT<ValueT>::EventId;
  using Event = EventT<ValueT>;
  using ReadsFromSource = typename ExecutionGraphT<ValueT>::ReadsFromSource;

  static constexpr EventId kNoSource = std::numeric_limits<EventId>::max();

  ExplorationGraphT() = default;

  // Add an event, tracking insertion order.
  [[nodiscard]] EventId add_event(ThreadId thread, EventLabelT<ValueT> label) {
    const auto id = graph_.add_event(thread, std::move(label));
    insertion_order_.push_back(id);
    insertion_position_[id] = insertion_order_.size() - 1U;
    porf_cache_ = nullptr;
    return id;
  }

  void set_reads_from(EventId receive_id, EventId source_id) {
    graph_.set_reads_from(receive_id, source_id);
    porf_cache_ = nullptr;
  }

  void set_reads_from_source(EventId receive_id, ReadsFromSource source) {
    graph_.set_reads_from_source(receive_id, std::move(source));
    porf_cache_ = nullptr;
  }

  void set_reads_from_bottom(EventId receive_id) {
    graph_.set_reads_from_bottom(receive_id);
    porf_cache_ = nullptr;
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

  // Returns true if the thread's last event is a BlockLabel or ErrorLabel.
  // DPOR's next-event selection skips such threads. Blocked receive threads
  // may later be rescheduled by the algorithm.
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
  [[nodiscard]] std::vector<ObservedValueT<ValueT>> thread_trace(ThreadId tid) const {
    // Gather events for this thread, sorted by event index (program order).
    std::vector<std::pair<EventIndex, EventId>> thread_events;
    for (EventId id = 0; id < event_count(); ++id) {
      const auto& evt = event(id);
      if (evt.thread == tid) {
        thread_events.emplace_back(evt.index, id);
      }
    }
    std::sort(thread_events.begin(), thread_events.end());

    std::vector<ObservedValueT<ValueT>> trace;
    const auto& rf = reads_from();

    for (const auto& [_, id] : thread_events) {
      const auto& evt = event(id);
      if (const auto* recv = as_receive(evt)) {
        auto it = rf.find(id);
        if (it != rf.end()) {
          if (it->second.is_bottom()) {
            trace.push_back(BottomValue{});
          } else {
            const auto& source_evt = event(it->second.send_id());
            if (const auto* send = as_send(source_evt)) {
              trace.push_back(send->value);
            }
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
    for (const auto& [recv_id, source] : rf) {
      auto recv_it = id_map.find(recv_id);
      if (recv_it == id_map.end()) {
        continue;
      }
      if (source.is_bottom()) {
        result.set_reads_from_bottom(recv_it->second);
        continue;
      }
      auto source_it = id_map.find(source.send_id());
      if (source_it != id_map.end()) {
        result.set_reads_from(recv_it->second, source_it->second);
      }
    }

    return result;
  }

  // Returns a copy with the rf assignment for recv changed to send.
  [[nodiscard]] ExplorationGraphT with_rf(EventId recv, EventId send) const {
    auto copy = *this;
    copy.set_reads_from(recv, send);
    return copy;
  }

  [[nodiscard]] ExplorationGraphT with_rf_source(EventId recv, ReadsFromSource source) const {
    auto copy = *this;
    copy.set_reads_from_source(recv, std::move(source));
    return copy;
  }

  [[nodiscard]] ExplorationGraphT with_bottom_rf(EventId recv) const {
    auto copy = *this;
    copy.set_reads_from_bottom(recv);
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
    if (!is_valid_event_id(from) || !is_valid_event_id(to)) {
      throw std::out_of_range("event id not found in exploration graph");
    }
    ensure_porf_cache();
    if (porf_cache_->has_cycle) {
      throw std::logic_error("porf_contains called on a graph with a causal cycle");
    }
    if (from == to) {
      return false;  // Acyclic graph: no self-loops in strict transitive closure.
    }
    const auto& cache = *porf_cache_;
    const auto ci_it = cache.thread_clock_index.find(event(from).thread);
    if (ci_it == cache.thread_clock_index.end()) {
      return false;
    }
    const auto ci = ci_it->second;
    return cache.clocks[to][ci] >= cache.position_in_thread[from] + 1;
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
    ensure_porf_cache();
    return porf_cache_->has_cycle;
  }

  // Access to underlying execution graph (for consistency checker etc.)
  [[nodiscard]] const ExecutionGraphT<ValueT>& execution_graph() const noexcept {
    return graph_;
  }

 private:
  ExecutionGraphT<ValueT> graph_{};
  std::vector<EventId> insertion_order_{};
  std::unordered_map<EventId, std::size_t> insertion_position_{};
  mutable std::shared_ptr<PorfCache> porf_cache_{};

  void ensure_porf_cache() const {
    if (porf_cache_) {
      return;
    }

    const auto n = event_count();
    auto cache = std::make_shared<PorfCache>();

    if (n == 0) {
      porf_cache_ = std::move(cache);
      return;
    }

    // Build per-thread event lists (sorted by event index for po order).
    std::unordered_map<ThreadId, std::vector<std::pair<EventIndex, EventId>>> thread_events;
    for (EventId id = 0; id < n; ++id) {
      const auto& evt = event(id);
      thread_events[evt.thread].emplace_back(evt.index, id);
    }

    // Assign dense clock indices per thread.
    for (auto& [tid, evts] : thread_events) {
      std::sort(evts.begin(), evts.end());
      cache->thread_clock_index[tid] = cache->num_threads++;
    }

    // Compute position_in_thread for each event.
    cache->position_in_thread.resize(n, 0);
    for (const auto& [tid, evts] : thread_events) {
      for (std::size_t pos = 0; pos < evts.size(); ++pos) {
        cache->position_in_thread[evts[pos].second] = pos;
      }
    }

    // Build adjacency list + in-degree array.
    std::vector<std::vector<EventId>> successors(n);
    std::vector<std::size_t> in_degree(n, 0);

    // po edges: consecutive events within each thread.
    for (const auto& [tid, evts] : thread_events) {
      for (std::size_t i = 1; i < evts.size(); ++i) {
        const auto pred = evts[i - 1].second;
        const auto succ = evts[i].second;
        successors[pred].push_back(succ);
        ++in_degree[succ];
      }
    }

    // rf edges: send -> recv (with same validation as rf_relation()).
    for (const auto& [recv_id, source] : reads_from()) {
      if (!is_valid_event_id(recv_id)) {
        throw std::invalid_argument("reads-from relation refers to an unknown receive event id");
      }
      if (!is_receive(event(recv_id))) {
        throw std::invalid_argument("reads-from relation target event is not a receive");
      }
      if (source.is_bottom()) {
        continue;
      }
      const auto source_id = source.send_id();
      if (!is_valid_event_id(source_id)) {
        throw std::invalid_argument("reads-from relation source refers to an unknown send event id");
      }
      if (!is_send(event(source_id))) {
        throw std::invalid_argument("reads-from relation source event is not a send");
      }
      successors[source_id].push_back(recv_id);
      ++in_degree[recv_id];
    }

    // Kahn's topological sort.
    std::queue<EventId> ready;
    for (EventId id = 0; id < n; ++id) {
      if (in_degree[id] == 0) {
        ready.push(id);
      }
    }

    std::vector<EventId> topo_order;
    topo_order.reserve(n);
    while (!ready.empty()) {
      const auto node = ready.front();
      ready.pop();
      topo_order.push_back(node);
      for (const auto succ : successors[node]) {
        if (--in_degree[succ] == 0) {
          ready.push(succ);
        }
      }
    }

    if (topo_order.size() < n) {
      cache->has_cycle = true;
      porf_cache_ = std::move(cache);
      return;
    }

    // Compute vector clocks in topological order.
    const auto width = cache->num_threads;
    cache->clocks.resize(n, std::vector<std::size_t>(width, 0));

    // Pre-compute rf target mapping: recv_id -> source_id.
    // All edges are already validated by the adjacency-building loop above.
    std::unordered_map<EventId, EventId> rf_source;
    for (const auto& [recv_id, source] : reads_from()) {
      if (source.is_send()) {
        rf_source[recv_id] = source.send_id();
      }
    }

    // Pre-compute po predecessor: for each event that has a po predecessor.
    std::unordered_map<EventId, EventId> po_pred;
    for (const auto& [tid, evts] : thread_events) {
      for (std::size_t i = 1; i < evts.size(); ++i) {
        po_pred[evts[i].second] = evts[i - 1].second;
      }
    }

    for (const auto id : topo_order) {
      auto& clock = cache->clocks[id];

      // Start with po-predecessor's clock.
      auto po_it = po_pred.find(id);
      if (po_it != po_pred.end()) {
        clock = cache->clocks[po_it->second];
      }

      // Join with rf source's clock (pointwise max).
      auto rf_it = rf_source.find(id);
      if (rf_it != rf_source.end()) {
        const auto& src_clock = cache->clocks[rf_it->second];
        for (std::size_t i = 0; i < width; ++i) {
          clock[i] = std::max(clock[i], src_clock[i]);
        }
      }

      // Set own position.
      const auto& evt = event(id);
      auto ci = cache->thread_clock_index[evt.thread];
      clock[ci] = cache->position_in_thread[id] + 1;
    }

    porf_cache_ = std::move(cache);
  }
};

using ExplorationGraph = ExplorationGraphT<Value>;

}  // namespace dpor::model
