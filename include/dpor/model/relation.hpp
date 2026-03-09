#pragma once

// Relation layer design rationale:
// - Keep relation *algebra* generic (`compose`, transitive closure, etc.).
// - Keep relation *storage* pluggable (explicit edges, derived per-thread order, etc.).
// - Use a compile-time concept (`Relation`) instead of runtime polymorphism to keep
//   algorithms type-safe and cheap while allowing multiple backends.
// - This is important for DPOR: `po`, `rf`, and derived relations should share
//   a common interface even when represented differently.

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace dpor::model {

using NodeId = std::size_t;

template <typename R>
concept Relation = requires(const R& relation, const NodeId from, const NodeId to) {
  { relation.node_count() } -> std::same_as<std::size_t>;
  { relation.contains(from, to) } -> std::same_as<bool>;
  relation.for_each_successor(from, [](NodeId) {});
};

// Explicit adjacency-list relation. Useful for directly materialized relations
// such as reads-from edges.
class ExplicitRelation {
 public:
  explicit ExplicitRelation(std::size_t node_count = 0) : successors_(node_count) {}

  [[nodiscard]] std::size_t node_count() const noexcept { return successors_.size(); }

  void set_node_count(std::size_t node_count) { successors_.assign(node_count, {}); }

  void add_edge(NodeId from, NodeId to) {
    validate_node(from);
    validate_node(to);
    auto& successors = successors_[from];
    if (std::find(successors.begin(), successors.end(), to) == successors.end()) {
      successors.push_back(to);
    }
  }

  [[nodiscard]] bool contains(NodeId from, NodeId to) const {
    if (!is_valid_node(from) || !is_valid_node(to)) {
      return false;
    }
    const auto& successors = successors_[from];
    return std::find(successors.begin(), successors.end(), to) != successors.end();
  }

  template <typename Func>
  void for_each_successor(NodeId from,
                          Func&& func) const {  // NOLINT(cppcoreguidelines-missing-std-forward)
    validate_node(from);
    for (const auto successor : successors_[from]) {
      func(successor);
    }
  }

 private:
  [[nodiscard]] bool is_valid_node(NodeId node) const noexcept { return node < successors_.size(); }

  void validate_node(NodeId node) const {
    if (!is_valid_node(node)) {
      throw std::out_of_range("relation node id is out of bounds");
    }
  }

  std::vector<std::vector<NodeId>> successors_;
};

// Program-order relation derived from per-thread event sequences.
// We do not store all transitive edges explicitly; membership and successor
// enumeration are computed from thread-local ordering metadata.
class ProgramOrderRelation {
 public:
  explicit ProgramOrderRelation(std::size_t node_count = 0)
      : node_count_(node_count),
        thread_of_(node_count, kNoThread),
        position_in_thread_(node_count, 0) {}

  ProgramOrderRelation(std::size_t node_count, std::vector<std::vector<NodeId>> thread_events)
      : ProgramOrderRelation(node_count) {
    set_thread_events(std::move(thread_events));
  }

  [[nodiscard]] std::size_t node_count() const noexcept { return node_count_; }

  void set_node_count(std::size_t node_count) {
    node_count_ = node_count;
    thread_events_.clear();
    thread_of_.assign(node_count_, kNoThread);
    position_in_thread_.assign(node_count_, 0);
  }

  void set_thread_events(std::vector<std::vector<NodeId>> thread_events) {
    thread_events_ = std::move(thread_events);
    rebuild_index();
  }

  [[nodiscard]] bool contains(NodeId from, NodeId to) const {
    if (!is_valid_node(from) || !is_valid_node(to)) {
      return false;
    }
    const auto from_thread = thread_of_[from];
    if (from_thread == kNoThread || from_thread != thread_of_[to]) {
      return false;
    }
    return position_in_thread_[from] < position_in_thread_[to];
  }

  template <typename Func>
  void for_each_successor(NodeId from,
                          Func&& func) const {  // NOLINT(cppcoreguidelines-missing-std-forward)
    validate_node(from);
    const auto thread = thread_of_[from];
    if (thread == kNoThread) {
      return;
    }

    const auto& events = thread_events_[thread];
    const auto from_position = position_in_thread_[from];
    for (std::size_t i = from_position + 1; i < events.size(); ++i) {
      func(events[i]);
    }
  }

 private:
  static constexpr std::size_t kNoThread = std::numeric_limits<std::size_t>::max();

  [[nodiscard]] bool is_valid_node(NodeId node) const noexcept { return node < node_count_; }

  void validate_node(NodeId node) const {
    if (!is_valid_node(node)) {
      throw std::out_of_range("relation node id is out of bounds");
    }
  }

  void rebuild_index() {
    thread_of_.assign(node_count_, kNoThread);
    position_in_thread_.assign(node_count_, 0);

    for (std::size_t thread = 0; thread < thread_events_.size(); ++thread) {
      const auto& events = thread_events_[thread];
      for (std::size_t position = 0; position < events.size(); ++position) {
        const auto event = events[position];
        validate_node(event);
        if (thread_of_[event] != kNoThread) {
          throw std::invalid_argument("event appears in more than one thread sequence");
        }
        thread_of_[event] = thread;
        position_in_thread_[event] = position;
      }
    }
  }

  std::size_t node_count_{0};
  std::vector<std::vector<NodeId>> thread_events_;
  std::vector<std::size_t> thread_of_;
  std::vector<std::size_t> position_in_thread_;
};

// Relational composition view: (left ; right).
// This is a lazy adapter, not a materialized relation.
template <Relation LeftRelation, Relation RightRelation>
class ComposeRelation {
 public:
  ComposeRelation(const LeftRelation& left, const RightRelation& right)
      : left_(left), right_(right) {
    if (left_.node_count() != right_.node_count()) {
      throw std::invalid_argument("cannot compose relations with different node counts");
    }
  }

  [[nodiscard]] std::size_t node_count() const noexcept { return left_.node_count(); }

  [[nodiscard]] bool contains(NodeId from, NodeId to) const {
    validate_node(from);
    validate_node(to);

    bool found = false;
    left_.for_each_successor(from, [&](const NodeId intermediate) {
      if (!found && right_.contains(intermediate, to)) {
        found = true;
      }
    });
    return found;
  }

  template <typename Func>
  void for_each_successor(NodeId from,
                          Func&& func) const {  // NOLINT(cppcoreguidelines-missing-std-forward)
    validate_node(from);

    std::vector<bool> emitted(node_count(), false);
    left_.for_each_successor(from, [&](const NodeId intermediate) {
      right_.for_each_successor(intermediate, [&](const NodeId successor) {
        if (!emitted[successor]) {
          emitted[successor] = true;
          func(successor);
        }
      });
    });
  }

 private:
  void validate_node(NodeId node) const {
    if (node >= node_count()) {
      throw std::out_of_range("relation node id is out of bounds");
    }
  }

  const LeftRelation& left_;
  const RightRelation& right_;
};

template <Relation LeftRelation, Relation RightRelation>
[[nodiscard]] inline auto compose(const LeftRelation& left, const RightRelation& right) {
  return ComposeRelation<LeftRelation, RightRelation>(left, right);
}

// Transitive-closure view: relation+ (non-reflexive).
// Reachability is computed on demand via graph traversal.
template <Relation BaseRelation>
class TransitiveClosureRelation {
 public:
  explicit TransitiveClosureRelation(const BaseRelation& relation) : relation_(relation) {}

  [[nodiscard]] std::size_t node_count() const noexcept { return relation_.node_count(); }

  [[nodiscard]] bool contains(NodeId from, NodeId to) const {
    validate_node(from);
    validate_node(to);

    bool found = false;
    for_each_successor(from, [&](const NodeId successor) {
      if (successor == to) {
        found = true;
      }
    });
    return found;
  }

  template <typename Func>
  void for_each_successor(NodeId from,
                          Func&& func) const {  // NOLINT(cppcoreguidelines-missing-std-forward)
    validate_node(from);

    std::vector<bool> visited(node_count(), false);
    std::vector<NodeId> pending{};

    relation_.for_each_successor(from,
                                 [&](const NodeId successor) { pending.push_back(successor); });

    std::size_t cursor = 0;
    while (cursor < pending.size()) {
      const auto current = pending[cursor++];
      if (visited[current]) {
        continue;
      }
      visited[current] = true;
      func(current);

      relation_.for_each_successor(current, [&](const NodeId successor) {
        if (!visited[successor]) {
          pending.push_back(successor);
        }
      });
    }
  }

 private:
  void validate_node(NodeId node) const {
    if (node >= node_count()) {
      throw std::out_of_range("relation node id is out of bounds");
    }
  }

  const BaseRelation& relation_;
};

template <Relation BaseRelation>
[[nodiscard]] inline auto transitive_closure(const BaseRelation& relation) {
  return TransitiveClosureRelation<BaseRelation>(relation);
}

// Union-relation view: left ∪ right.
// Contains (from, to) if either left or right contains it.
template <Relation LeftRelation, Relation RightRelation>
class UnionRelation {
 public:
  UnionRelation(const LeftRelation& left, const RightRelation& right) : left_(left), right_(right) {
    if (left_.node_count() != right_.node_count()) {
      throw std::invalid_argument("cannot union relations with different node counts");
    }
  }

  [[nodiscard]] std::size_t node_count() const noexcept { return left_.node_count(); }

  [[nodiscard]] bool contains(NodeId from, NodeId to) const {
    validate_node(from);
    validate_node(to);
    return left_.contains(from, to) || right_.contains(from, to);
  }

  template <typename Func>
  void for_each_successor(NodeId from,
                          Func&& func) const {  // NOLINT(cppcoreguidelines-missing-std-forward)
    validate_node(from);

    std::vector<bool> emitted(node_count(), false);
    left_.for_each_successor(from, [&](const NodeId successor) {
      if (!emitted[successor]) {
        emitted[successor] = true;
        func(successor);
      }
    });
    right_.for_each_successor(from, [&](const NodeId successor) {
      if (!emitted[successor]) {
        emitted[successor] = true;
        func(successor);
      }
    });
  }

 private:
  void validate_node(NodeId node) const {
    if (node >= node_count()) {
      throw std::out_of_range("relation node id is out of bounds");
    }
  }

  const LeftRelation& left_;
  const RightRelation& right_;
};

template <Relation L, Relation R>
[[nodiscard]] inline auto relation_union(const L& left, const R& right) {
  return UnionRelation<L, R>(left, right);
}

}  // namespace dpor::model
