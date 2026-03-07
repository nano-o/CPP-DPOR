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
// - has_porf_cache(): reports whether PORF reachability has been materialized
// - is_known_acyclic(): reports whether append-only forward-path mutations have
//   preserved a known-acyclic (po ∪ rf) structure
// - has_causal_cycle_without_cache(): cycle-only check without building PORF cache
// - receives_in_destination(send_id): receives in the send's destination thread
// - has_causal_cycle(): cache-backed cycle check
//
// Graph operations (restrict, with_rf, with_nd_value) return copies.
// This matches the recursive branching nature of Algorithm 1.

#include "dpor/model/event.hpp"
#include "dpor/model/execution_graph.hpp"
#include "dpor/model/relation.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::model {

// Per-thread structural metadata maintained incrementally by add_event().
// Enables O(1) thread_event_count(), thread_is_terminated(), and last_event_id().
struct ThreadState {
  std::size_t event_count{0};
  std::size_t last_event_id{std::numeric_limits<std::size_t>::max()};
};

struct PorfCache {
  std::vector<std::vector<std::size_t>> clocks;
  std::vector<std::size_t> position_in_thread;
  std::vector<std::size_t> thread_clock_index;
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

  // Add an event, tracking insertion order and per-thread metadata.
  [[nodiscard]] EventId add_event(ThreadId thread, EventLabelT<ValueT> label) {
    std::optional<EventId> po_predecessor;
    const auto thread_index = static_cast<std::size_t>(thread);
    if (thread_index < thread_state_.size() &&
        thread_state_[thread_index].event_count != 0) {
      po_predecessor = thread_state_[thread_index].last_event_id;
    }
    auto parent_cache = current_append_parent_cache();

    const auto id = graph_.add_event(thread, std::move(label));
    insertion_order_.push_back(id);
    assert(id == insertion_position_.size());
    insertion_position_.push_back(insertion_order_.size() - 1U);
    porf_cache_ = nullptr;

    if (thread_index >= thread_state_.size()) {
      thread_state_.resize(thread_index + 1);
    }
    auto& ts = thread_state_[thread];
    assert(ts.last_event_id == kNoSource || event(id).index > event(ts.last_event_id).index);
    ts.event_count++;
    ts.last_event_id = id;
    update_acyclicity_after_add_event(id, po_predecessor, std::move(parent_cache));
    return id;
  }

  void set_reads_from(EventId receive_id, EventId source_id) {
    graph_.set_reads_from(receive_id, source_id);
    porf_cache_ = nullptr;
    update_acyclicity_after_rf_assignment(receive_id);
  }

  void set_reads_from_source(EventId receive_id, ReadsFromSource source) {
    graph_.set_reads_from_source(receive_id, std::move(source));
    porf_cache_ = nullptr;
    update_acyclicity_after_rf_assignment(receive_id);
  }

  void set_reads_from_bottom(EventId receive_id) {
    graph_.set_reads_from_bottom(receive_id);
    porf_cache_ = nullptr;
    update_acyclicity_after_rf_assignment(receive_id);
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
    if (a >= insertion_position_.size() || b >= insertion_position_.size()) {
      throw std::out_of_range("event id not found in insertion order");
    }
    return insertion_position_[a] <= insertion_position_[b];
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
    const auto thread_index = static_cast<std::size_t>(tid);
    if (thread_index >= thread_state_.size() ||
        thread_state_[thread_index].event_count == 0) {
      return false;  // No events for this thread yet.
    }
    const auto& last_evt = event(thread_state_[thread_index].last_event_id);
    return is_block(last_evt) || is_error(last_evt);
  }

  // Count events belonging to the given thread.
  [[nodiscard]] std::size_t thread_event_count(ThreadId tid) const {
    const auto thread_index = static_cast<std::size_t>(tid);
    if (thread_index >= thread_state_.size()) {
      return 0;
    }
    return thread_state_[thread_index].event_count;
  }

  // Returns the last event ID for the given thread, or kNoSource if no events.
  [[nodiscard]] EventId last_event_id(ThreadId tid) const {
    const auto thread_index = static_cast<std::size_t>(tid);
    if (thread_index >= thread_state_.size() ||
        thread_state_[thread_index].event_count == 0) {
      return kNoSource;
    }
    return thread_state_[thread_index].last_event_id;
  }

  // Extract the value sequence visible to a thread: values from receives (via rf)
  // and ND choices, in program order.
  // Per-thread events appear in monotonic index order in the events vector
  // (guaranteed by add_event's auto-indexing), so no sort is needed.
  [[nodiscard]] std::vector<ObservedValueT<ValueT>> thread_trace(ThreadId tid) const {
    std::vector<ObservedValueT<ValueT>> trace;
    trace.reserve(thread_event_count(tid));
    const auto& rf = reads_from();

    for (EventId id = 0; id < event_count(); ++id) {
      const auto& evt = event(id);
      if (evt.thread != tid) {
        continue;
      }
      if (const auto* recv = as_receive(evt)) {
        auto rf_it = rf.find(id);
        if (rf_it != rf.end()) {
          if (rf_it->second.is_bottom()) {
            trace.push_back(BottomValue{});
          } else {
            const auto& source_evt = event(rf_it->second.send_id());
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

    result.invalidate_known_acyclicity();
    return result;
  }

  // Returns a copy with the rf assignment for recv changed to send.
  [[nodiscard]] ExplorationGraphT with_rf(EventId recv, EventId send) const {
    auto copy = *this;
    copy.invalidate_known_acyclicity();
    copy.set_reads_from(recv, send);
    return copy;
  }

  [[nodiscard]] ExplorationGraphT with_rf_source(EventId recv, ReadsFromSource source) const {
    auto copy = *this;
    copy.invalidate_known_acyclicity();
    copy.set_reads_from_source(recv, std::move(source));
    return copy;
  }

  [[nodiscard]] ExplorationGraphT with_bottom_rf(EventId recv) const {
    auto copy = *this;
    copy.invalidate_known_acyclicity();
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
    const auto thread_index = static_cast<std::size_t>(event(from).thread);
    if (thread_index >= cache.thread_clock_index.size() ||
        cache.thread_clock_index[thread_index] == kNoSource) {
      return false;
    }
    const auto ci = cache.thread_clock_index[thread_index];
    return cache.clocks[to][ci] >= cache.position_in_thread[from] + 1;
  }

  [[nodiscard]] bool has_porf_cache() const noexcept {
    return static_cast<bool>(porf_cache_);
  }

  // This tracks only the narrow append-only fast path. The checker still runs
  // full endpoint/RF validation before using it.
  [[nodiscard]] bool is_known_acyclic() const noexcept {
    return known_acyclic_;
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

  // Cycle-only check on (po ∪ rf) without materializing vector clocks.
  [[nodiscard]] bool has_causal_cycle_without_cache() const {
    const auto porf_graph = build_porf_graph_structure();
    const auto topo_order =
        compute_topological_order(porf_graph.successors, porf_graph.in_degree);
    return topo_order.size() < event_count();
  }

  // Cache-backed causal cycle check on (po ∪ rf).
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
  std::vector<std::size_t> insertion_position_{};
  std::vector<ThreadState> thread_state_{};
  bool known_acyclic_{true};
  struct AppendProvenance {
    EventId appended_event_id{};
    std::optional<EventId> po_predecessor_id{};
    std::optional<ReadsFromSource> rf_source{};
    bool awaiting_rf_assignment{false};
    std::shared_ptr<const PorfCache> parent_cache{};
  };
  std::optional<AppendProvenance> append_provenance_{};
  mutable std::shared_ptr<PorfCache> porf_cache_{};

  struct PorfGraphStructure {
    std::vector<std::vector<EventId>> thread_events{};
    std::vector<std::vector<EventId>> successors{};
    std::vector<std::size_t> in_degree{};
  };

  void invalidate_known_acyclicity() {
    known_acyclic_ = false;
    append_provenance_.reset();
  }

  [[nodiscard]] std::shared_ptr<const PorfCache> current_append_parent_cache() const {
    if (!known_acyclic_ || !porf_cache_ || porf_cache_->has_cycle) {
      return nullptr;
    }
    return porf_cache_;
  }

  void update_acyclicity_after_add_event(
      EventId event_id,
      std::optional<EventId> po_predecessor_id,
      std::shared_ptr<const PorfCache> parent_cache) {
    if (!known_acyclic_) {
      append_provenance_.reset();
      return;
    }

    append_provenance_ = AppendProvenance{
        .appended_event_id = event_id,
        .po_predecessor_id = po_predecessor_id,
        .parent_cache = std::move(parent_cache),
    };
    if (is_receive(event(event_id))) {
      append_provenance_->awaiting_rf_assignment = true;
    }
  }

  void update_acyclicity_after_rf_assignment(EventId receive_id) {
    if (known_acyclic_ &&
        append_provenance_.has_value() &&
        append_provenance_->appended_event_id == receive_id &&
        append_provenance_->awaiting_rf_assignment &&
        is_receive(event(receive_id))) {
      append_provenance_->awaiting_rf_assignment = false;
      append_provenance_->rf_source = reads_from().at(receive_id);
      return;
    }
    invalidate_known_acyclicity();
  }

  [[nodiscard]] std::shared_ptr<PorfCache> try_extend_porf_cache_from_parent() const {
    if (!known_acyclic_ || !append_provenance_.has_value()) {
      return nullptr;
    }

    const auto& provenance = *append_provenance_;
    if (!provenance.parent_cache || provenance.parent_cache->has_cycle) {
      return nullptr;
    }

    const auto parent_event_count = provenance.parent_cache->position_in_thread.size();
    if (event_count() != parent_event_count + 1 ||
        provenance.appended_event_id != parent_event_count) {
      return nullptr;
    }

    if (provenance.awaiting_rf_assignment) {
      return nullptr;
    }

    if (!is_valid_event_id(provenance.appended_event_id)) {
      return nullptr;
    }

    const auto& fresh_event = event(provenance.appended_event_id);
    if (is_receive(fresh_event) && !provenance.rf_source.has_value()) {
      return nullptr;
    }

    auto cache = std::make_shared<PorfCache>(*provenance.parent_cache);
    if (cache->clocks.size() != parent_event_count ||
        cache->position_in_thread.size() != parent_event_count) {
      return nullptr;
    }

    const auto thread_index = static_cast<std::size_t>(fresh_event.thread);
    if (thread_index >= cache->thread_clock_index.size()) {
      cache->thread_clock_index.resize(thread_index + 1, kNoSource);
    }

    const bool first_event_in_thread = cache->thread_clock_index[thread_index] == kNoSource;
    if (first_event_in_thread) {
      cache->thread_clock_index[thread_index] = cache->num_threads++;
      for (auto& clock : cache->clocks) {
        clock.push_back(0);
      }
    }

    std::size_t position = 0;
    if (provenance.po_predecessor_id.has_value()) {
      const auto pred_id = *provenance.po_predecessor_id;
      if (pred_id >= parent_event_count ||
          event(pred_id).thread != fresh_event.thread) {
        return nullptr;
      }
      position = cache->position_in_thread[pred_id] + 1;
    } else if (fresh_event.index != 0) {
      return nullptr;
    }

    const auto width = cache->num_threads;
    cache->position_in_thread.push_back(position);
    cache->clocks.push_back(std::vector<std::size_t>(width, 0));

    auto& clock = cache->clocks.back();
    if (provenance.po_predecessor_id.has_value()) {
      clock = cache->clocks[*provenance.po_predecessor_id];
    }

    if (provenance.rf_source.has_value() && provenance.rf_source->is_send()) {
      const auto source_id = provenance.rf_source->send_id();
      if (source_id >= parent_event_count) {
        throw std::invalid_argument(
            "reads-from relation source refers to an unknown send event id");
      }
      if (!is_send(event(source_id))) {
        throw std::invalid_argument("reads-from relation source event is not a send");
      }
      const auto& source_clock = cache->clocks[source_id];
      for (std::size_t i = 0; i < width; ++i) {
        clock[i] = std::max(clock[i], source_clock[i]);
      }
    }

    const auto ci = cache->thread_clock_index[thread_index];
    clock[ci] = position + 1;
    cache->has_cycle = false;
    return cache;
  }

  [[nodiscard]] std::shared_ptr<PorfCache> build_full_porf_cache() const {
    const auto n = event_count();
    auto cache = std::make_shared<PorfCache>();

    if (n == 0) {
      return cache;
    }

    auto porf_graph = build_porf_graph_structure();

    // Assign dense clock indices per thread.
    cache->thread_clock_index.assign(porf_graph.thread_events.size(), kNoSource);
    for (std::size_t tid = 0; tid < porf_graph.thread_events.size(); ++tid) {
      const auto& evts = porf_graph.thread_events[tid];
      if (evts.empty()) {
        continue;
      }
      cache->thread_clock_index[tid] = cache->num_threads++;
    }

    // Compute position_in_thread for each event.
    cache->position_in_thread.resize(n, 0);
    for (const auto& evts : porf_graph.thread_events) {
      for (std::size_t pos = 0; pos < evts.size(); ++pos) {
        cache->position_in_thread[evts[pos]] = pos;
      }
    }

    const auto topo_order =
        compute_topological_order(porf_graph.successors, porf_graph.in_degree);

    if (topo_order.size() < n) {
      cache->has_cycle = true;
      return cache;
    }

    // Compute vector clocks in topological order.
    const auto width = cache->num_threads;
    cache->clocks.resize(n, std::vector<std::size_t>(width, 0));

    // Pre-compute rf target mapping: recv_id -> source_id.
    // All edges are already validated by the adjacency-building loop above.
    std::unordered_map<EventId, EventId> rf_source;
    rf_source.reserve(reads_from().size());
    for (const auto& [recv_id, source] : reads_from()) {
      if (source.is_send()) {
        rf_source[recv_id] = source.send_id();
      }
    }

    // Pre-compute po predecessor: for each event that has a po predecessor.
    std::unordered_map<EventId, EventId> po_pred;
    po_pred.reserve(n);
    for (const auto& evts : porf_graph.thread_events) {
      for (std::size_t i = 1; i < evts.size(); ++i) {
        po_pred[evts[i]] = evts[i - 1];
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
      const auto ci = cache->thread_clock_index[evt.thread];
      clock[ci] = cache->position_in_thread[id] + 1;
    }

    return cache;
  }

  // ExplorationGraphT only grows through add_event(), which assigns fresh
  // monotonic per-thread indices. Scanning events in ID order therefore
  // already yields each thread's immediate program order.
  [[nodiscard]] std::vector<std::vector<EventId>> build_thread_events() const {
    std::vector<std::vector<EventId>> thread_events(thread_state_.size());
    for (EventId id = 0; id < event_count(); ++id) {
      const auto thread_index = static_cast<std::size_t>(event(id).thread);
      if (thread_index >= thread_events.size()) {
        thread_events.resize(thread_index + 1);
      }
      thread_events[thread_index].push_back(id);
    }
    return thread_events;
  }

  static void add_po_edges(
      const std::vector<std::vector<EventId>>& thread_events,
      std::vector<std::vector<EventId>>& successors,
      std::vector<std::size_t>& in_degree) {
    for (const auto& events : thread_events) {
      for (std::size_t i = 1; i < events.size(); ++i) {
        const auto pred = events[i - 1];
        const auto succ = events[i];
        successors[pred].push_back(succ);
        ++in_degree[succ];
      }
    }
  }

  void add_rf_edges(
      std::vector<std::vector<EventId>>& successors,
      std::vector<std::size_t>& in_degree) const {
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
  }

  [[nodiscard]] PorfGraphStructure build_porf_graph_structure() const {
    PorfGraphStructure porf_graph;
    porf_graph.thread_events = build_thread_events();
    porf_graph.successors.assign(event_count(), {});
    porf_graph.in_degree.assign(event_count(), 0);
    add_po_edges(porf_graph.thread_events, porf_graph.successors, porf_graph.in_degree);
    add_rf_edges(porf_graph.successors, porf_graph.in_degree);
    return porf_graph;
  }

  [[nodiscard]] static std::vector<EventId> compute_topological_order(
      const std::vector<std::vector<EventId>>& successors,
      std::vector<std::size_t> in_degree) {
    std::queue<EventId> ready;
    for (EventId id = 0; id < successors.size(); ++id) {
      if (in_degree[id] == 0) {
        ready.push(id);
      }
    }

    std::vector<EventId> topo_order;
    topo_order.reserve(successors.size());
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
    return topo_order;
  }

  void ensure_porf_cache() const {
    if (porf_cache_) {
      return;
    }
    if (const auto extended_cache = try_extend_porf_cache_from_parent()) {
      porf_cache_ = std::move(extended_cache);
      return;
    }
    porf_cache_ = build_full_porf_cache();
  }
};

using ExplorationGraph = ExplorationGraphT<Value>;

}  // namespace dpor::model
