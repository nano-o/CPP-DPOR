#pragma once

// ExplorationGraphT: insertion-ordered execution graph with DPOR operations.
//
// Wraps ExecutionGraphT<V> and adds:
// - Insertion order tracking (needed for <=_G and backward revisiting)
// - restrict(keep_set): returns a new graph with only specified events
// - with_rf(recv, send): returns a copy with rf assignment changed
// - with_nd_value(nd_event, value): returns a copy with ND choice value set
// - checkpoint()/rollback(): worker-local temporary mutation support
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::model {

template <typename ValueT>
class ExplorationGraphT;

namespace detail {

template <typename ValueT>
[[nodiscard]] ExplorationGraphT<ValueT> restrict_masked(
    const ExplorationGraphT<ValueT>& graph,
    const std::vector<std::uint8_t>& keep_mask);

}  // namespace detail

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
  ExplorationGraphT(const ExplorationGraphT& other)
      : graph_(other.graph_),
        insertion_order_(other.insertion_order_),
        insertion_position_(other.insertion_position_),
        thread_state_(other.thread_state_),
        known_acyclic_(other.known_acyclic_),
        pending_fresh_receive_id_(other.pending_fresh_receive_id_),
        porf_cache_(other.porf_cache_) {}
  ExplorationGraphT(ExplorationGraphT&&) noexcept = default;

  ExplorationGraphT& operator=(const ExplorationGraphT& other) {
    if (this == &other) {
      return *this;
    }

    graph_ = other.graph_;
    insertion_order_ = other.insertion_order_;
    insertion_position_ = other.insertion_position_;
    thread_state_ = other.thread_state_;
    known_acyclic_ = other.known_acyclic_;
    pending_fresh_receive_id_ = other.pending_fresh_receive_id_;
    porf_cache_ = other.porf_cache_;
    clear_worker_local_history();
    return *this;
  }

  ExplorationGraphT& operator=(ExplorationGraphT&&) noexcept = default;

  struct Checkpoint {
    std::size_t event_undo_size{0};
    std::size_t rf_undo_size{0};
    bool known_acyclic{true};
    std::optional<EventId> pending_fresh_receive_id{};
  };

  class ScopedRollback {
   public:
    explicit ScopedRollback(ExplorationGraphT& graph)
        : graph_(&graph), checkpoint_(graph.checkpoint()) {}

    ScopedRollback(const ScopedRollback&) = delete;
    ScopedRollback& operator=(const ScopedRollback&) = delete;

    ScopedRollback(ScopedRollback&& other) noexcept
        : graph_(std::exchange(other.graph_, nullptr)),
          checkpoint_(other.checkpoint_) {}

    ScopedRollback& operator=(ScopedRollback&& other) noexcept {
      if (this == &other) {
        return *this;
      }
      if (graph_ != nullptr) {
        graph_->rollback(checkpoint_);
      }
      graph_ = std::exchange(other.graph_, nullptr);
      checkpoint_ = other.checkpoint_;
      return *this;
    }

    ~ScopedRollback() {
      if (graph_ != nullptr) {
        graph_->rollback(checkpoint_);
      }
    }

    void release() noexcept {
      graph_ = nullptr;
    }

   private:
    ExplorationGraphT* graph_{nullptr};
    Checkpoint checkpoint_{};
  };

  [[nodiscard]] Checkpoint checkpoint() const noexcept {
    return Checkpoint{
        .event_undo_size = event_undo_log_.size(),
        .rf_undo_size = rf_undo_log_.size(),
        .known_acyclic = known_acyclic_,
        .pending_fresh_receive_id = pending_fresh_receive_id_,
    };
  }

  void rollback(Checkpoint checkpoint) {
    if (checkpoint.event_undo_size > event_undo_log_.size() ||
        checkpoint.rf_undo_size > rf_undo_log_.size()) {
      throw std::logic_error("checkpoint does not belong to the current graph state");
    }

    while (rf_undo_log_.size() > checkpoint.rf_undo_size) {
      undo_last_rf_assignment();
    }
    while (event_undo_log_.size() > checkpoint.event_undo_size) {
      undo_last_event_append();
    }

    known_acyclic_ = checkpoint.known_acyclic;
    pending_fresh_receive_id_ = std::move(checkpoint.pending_fresh_receive_id);
    porf_cache_ = nullptr;
  }

  // Add an event, tracking insertion order and per-thread metadata.
  [[nodiscard]] EventId add_event(ThreadId thread, EventLabelT<ValueT> label) {
    const auto thread_index = static_cast<std::size_t>(thread);
    const auto previous_thread_state =
        thread_index < thread_state_.size() ? thread_state_[thread_index] : ThreadState{};
    const auto previous_next_event_index =
        thread_index < graph_.next_event_index_by_thread_.size()
        ? graph_.next_event_index_by_thread_[thread_index]
        : 0;
    const auto id = graph_.add_event(thread, std::move(label));
    event_undo_log_.push_back(EventUndo{
        .event_id = id,
        .thread = thread,
        .event_index = event(id).index,
        .previous_thread_state = previous_thread_state,
        .previous_next_event_index = previous_next_event_index,
    });
    insertion_order_.push_back(id);
    assert(id == insertion_position_.size());
    insertion_position_.push_back(insertion_order_.size() - 1U);
    porf_cache_ = nullptr;

    if (thread_index >= thread_state_.size()) {
      thread_state_.resize(thread_index + 1);
    }
    auto& ts = thread_state_[thread_index];
    assert(ts.last_event_id == kNoSource || event(id).index > event(ts.last_event_id).index);
    ts.event_count++;
    ts.last_event_id = id;
    update_acyclicity_after_add_event(id);
    return id;
  }

  void set_reads_from(EventId receive_id, EventId source_id) {
    set_reads_from_source(receive_id, ReadsFromSource::from_send(source_id));
  }

  void set_reads_from_source(EventId receive_id, ReadsFromSource source) {
    const auto previous_source = current_reads_from_source(receive_id);
    const auto source_copy = source;
    graph_.set_reads_from_source(receive_id, std::move(source));
    rf_undo_log_.push_back(ReadsFromUndo{
        .receive_id = receive_id,
        .previous_source = previous_source,
    });
    porf_cache_ = nullptr;
    update_acyclicity_after_rf_assignment(receive_id, source_copy);
  }

  void set_reads_from_bottom(EventId receive_id) {
    set_reads_from_source(receive_id, ReadsFromSource::bottom());
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
    std::vector<std::uint8_t> keep_mask(event_count(), 0);
    for (const auto kept_id : keep_set) {
      if (kept_id < keep_mask.size()) {
        keep_mask[kept_id] = 1;
      }
    }
    return restrict_from_keep_mask(keep_mask);
  }

  // Returns a copy with the rf assignment for recv changed to send.
  [[nodiscard]] ExplorationGraphT with_rf(EventId recv, EventId send) const {
    auto copy = *this;
    copy.invalidate_known_acyclicity();
    copy.set_reads_from(recv, send);
    copy.clear_worker_local_history();
    return copy;
  }

  [[nodiscard]] ExplorationGraphT with_rf_source(EventId recv, ReadsFromSource source) const {
    auto copy = *this;
    copy.invalidate_known_acyclicity();
    copy.set_reads_from_source(recv, std::move(source));
    copy.clear_worker_local_history();
    return copy;
  }

  [[nodiscard]] ExplorationGraphT with_bottom_rf(EventId recv) const {
    auto copy = *this;
    copy.invalidate_known_acyclicity();
    copy.set_reads_from_bottom(recv);
    copy.clear_worker_local_history();
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
  template <typename U>
  friend ExplorationGraphT<U> detail::restrict_masked(
      const ExplorationGraphT<U>& graph,
      const std::vector<std::uint8_t>& keep_mask);

  ExecutionGraphT<ValueT> graph_{};
  std::vector<EventId> insertion_order_{};
  std::vector<std::size_t> insertion_position_{};
  std::vector<ThreadState> thread_state_{};
  bool known_acyclic_{true};
  std::optional<EventId> pending_fresh_receive_id_{};
  mutable std::shared_ptr<PorfCache> porf_cache_{};

  struct EventUndo {
    EventId event_id{kNoSource};
    ThreadId thread{};
    EventIndex event_index{};
    ThreadState previous_thread_state{};
    EventIndex previous_next_event_index{};
  };

  struct ReadsFromUndo {
    EventId receive_id{kNoSource};
    std::optional<ReadsFromSource> previous_source{};
  };

  std::vector<EventUndo> event_undo_log_{};
  std::vector<ReadsFromUndo> rf_undo_log_{};

  struct PorfGraphStructure {
    std::vector<std::vector<EventId>> thread_events{};
    std::vector<std::vector<EventId>> successors{};
    std::vector<std::size_t> in_degree{};
  };

  void invalidate_known_acyclicity() {
    known_acyclic_ = false;
    pending_fresh_receive_id_.reset();
  }

  void clear_worker_local_history() {
    event_undo_log_.clear();
    rf_undo_log_.clear();
  }

  [[nodiscard]] ExplorationGraphT restrict_from_keep_mask(
      const std::vector<std::uint8_t>& keep_mask) const {
    if (keep_mask.size() != event_count()) {
      throw std::invalid_argument("keep mask size must match event count");
    }

    std::vector<EventId> kept_ids;
    kept_ids.reserve(std::count(keep_mask.begin(), keep_mask.end(), std::uint8_t{1}));
    std::vector<EventId> id_map(event_count(), kNoSource);

    EventId new_id = 0;
    for (const auto old_id : insertion_order_) {
      if (keep_mask[old_id] == 0U) {
        continue;
      }
      kept_ids.push_back(old_id);
      id_map[old_id] = new_id++;
    }

    ExplorationGraphT result;

    // Re-insert events in insertion order with remapped IDs.
    for (const auto old_id : kept_ids) {
      const auto& evt = event(old_id);
      auto label = evt.label;

      // Remap send destination: destinations are thread IDs, not event IDs,
      // so they stay unchanged.
      const auto remapped_id = result.add_event(evt.thread, std::move(label));
      static_cast<void>(remapped_id);
    }

    // Remap reads-from edges.
    const auto& rf = reads_from();
    for (const auto& [recv_id, source] : rf) {
      const auto remapped_recv = id_map[recv_id];
      if (remapped_recv == kNoSource) {
        continue;
      }
      if (source.is_bottom()) {
        result.set_reads_from_bottom(remapped_recv);
        continue;
      }
      const auto remapped_source = id_map[source.send_id()];
      if (remapped_source != kNoSource) {
        result.set_reads_from(remapped_recv, remapped_source);
      }
    }

    result.invalidate_known_acyclicity();
    result.clear_worker_local_history();
    return result;
  }

  [[nodiscard]] std::optional<ReadsFromSource> current_reads_from_source(EventId receive_id) const {
    const auto it = reads_from().find(receive_id);
    if (it == reads_from().end()) {
      return std::nullopt;
    }
    return it->second;
  }

  void undo_last_rf_assignment() {
    if (rf_undo_log_.empty()) {
      throw std::logic_error("rf undo log is empty");
    }

    const auto undo = rf_undo_log_.back();
    rf_undo_log_.pop_back();
    graph_.rollback_reads_from(undo.receive_id, undo.previous_source);
    porf_cache_ = nullptr;
  }

  void undo_last_event_append() {
    if (event_undo_log_.empty()) {
      throw std::logic_error("event undo log is empty");
    }

    const auto undo = event_undo_log_.back();
    event_undo_log_.pop_back();

    if (insertion_order_.empty() || insertion_order_.back() != undo.event_id) {
      throw std::logic_error("event undo log does not match insertion order tail");
    }
    if (insertion_position_.size() != graph_.events().size()) {
      throw std::logic_error("event undo log does not match insertion-position state");
    }

    graph_.rollback_last_event(
        undo.event_id,
        undo.thread,
        undo.event_index,
        undo.previous_next_event_index);
    insertion_order_.pop_back();
    insertion_position_.pop_back();

    const auto thread_index = static_cast<std::size_t>(undo.thread);
    if (thread_index >= thread_state_.size()) {
      throw std::logic_error("event undo log thread state missing");
    }
    thread_state_[thread_index] = undo.previous_thread_state;
    porf_cache_ = nullptr;
  }

  void update_acyclicity_after_add_event(EventId event_id) {
    pending_fresh_receive_id_.reset();
    if (known_acyclic_ && is_receive(event(event_id))) {
      pending_fresh_receive_id_ = event_id;
    }
  }

  void update_acyclicity_after_rf_assignment(
      EventId receive_id,
      const ReadsFromSource& source) {
    if (!known_acyclic_ ||
        !pending_fresh_receive_id_.has_value() ||
        *pending_fresh_receive_id_ != receive_id ||
        !is_valid_event_id(receive_id) ||
        !is_receive(event(receive_id))) {
      invalidate_known_acyclicity();
      return;
    }

    if (source.is_bottom()) {
      pending_fresh_receive_id_.reset();
      return;
    }

    const auto source_id = source.send_id();
    if (!is_valid_event_id(source_id) || !is_send(event(source_id))) {
      invalidate_known_acyclicity();
      return;
    }

    pending_fresh_receive_id_.reset();
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
    graph_.for_each_validated_rf_edge([&](const EventId source_id, const EventId recv_id) {
      successors[source_id].push_back(recv_id);
      ++in_degree[recv_id];
    });
  }

  [[nodiscard]] std::vector<std::size_t> compute_successor_out_degree(
      const std::vector<std::vector<EventId>>& thread_events) const {
    std::vector<std::size_t> out_degree(event_count(), 0);
    for (const auto& events : thread_events) {
      for (std::size_t i = 1; i < events.size(); ++i) {
        ++out_degree[events[i - 1]];
      }
    }
    graph_.for_each_validated_rf_edge([&](const EventId source_id, const EventId /*recv_id*/) {
      ++out_degree[source_id];
    });
    return out_degree;
  }

  [[nodiscard]] PorfGraphStructure build_porf_graph_structure() const {
    PorfGraphStructure porf_graph;
    porf_graph.thread_events = build_thread_events();
    porf_graph.successors.assign(event_count(), {});
    porf_graph.in_degree.assign(event_count(), 0);
    const auto out_degree = compute_successor_out_degree(porf_graph.thread_events);
    for (EventId id = 0; id < event_count(); ++id) {
      porf_graph.successors[id].reserve(out_degree[id]);
    }
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

    const auto n = event_count();
    auto cache = std::make_shared<PorfCache>();

    if (n == 0) {
      porf_cache_ = std::move(cache);
      return;
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
      porf_cache_ = std::move(cache);
      return;
    }

    // Compute vector clocks in topological order.
    const auto width = cache->num_threads;
    cache->clocks.resize(n, std::vector<std::size_t>(width, 0));

    // Pre-compute rf target mapping: recv_id -> source_id.
    // All edges are already validated by the adjacency-building loop above.
    std::vector<EventId> rf_source(n, kNoSource);
    for (const auto& [recv_id, source] : reads_from()) {
      if (source.is_send()) {
        rf_source[recv_id] = source.send_id();
      }
    }

    // Pre-compute po predecessor: for each event that has a po predecessor.
    std::vector<EventId> po_pred(n, kNoSource);
    for (const auto& evts : porf_graph.thread_events) {
      for (std::size_t i = 1; i < evts.size(); ++i) {
        po_pred[evts[i]] = evts[i - 1];
      }
    }

    for (const auto id : topo_order) {
      auto& clock = cache->clocks[id];

      // Start with po-predecessor's clock.
      if (po_pred[id] != kNoSource) {
        clock = cache->clocks[po_pred[id]];
      }

      // Join with rf source's clock (pointwise max).
      if (rf_source[id] != kNoSource) {
        const auto& src_clock = cache->clocks[rf_source[id]];
        for (std::size_t i = 0; i < width; ++i) {
          clock[i] = std::max(clock[i], src_clock[i]);
        }
      }

      // Set own position.
      const auto& evt = event(id);
      const auto ci = cache->thread_clock_index[evt.thread];
      clock[ci] = cache->position_in_thread[id] + 1;
    }

    porf_cache_ = std::move(cache);
  }
};

namespace detail {

template <typename ValueT>
[[nodiscard]] ExplorationGraphT<ValueT> restrict_masked(
    const ExplorationGraphT<ValueT>& graph,
    const std::vector<std::uint8_t>& keep_mask) {
  return graph.restrict_from_keep_mask(keep_mask);
}

}  // namespace detail

using ExplorationGraph = ExplorationGraphT<Value>;

}  // namespace dpor::model
