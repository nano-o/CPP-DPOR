#pragma once

#include "dpor/model/exploration_graph.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dpor::model {

enum class ConsistencyIssueCode : std::uint8_t {
  InvalidEventReference,
  ReadsFromTargetNotReceive,
  ReadsFromSourceNotSend,
  MissingReadsFromForReceive,
  BlockingReceiveReadsBottom,
  SendConsumedMultipleTimes,
  ReceiveDestinationMismatch,
  ReceiveValueMismatch,
  CausalCycle,
  FifoP2PUnreadEarlierMatchingSend,
  FifoP2PReceiveOrderViolation,
};

struct ConsistencyIssue {
  ConsistencyIssueCode code{};
  std::string message;
};

struct ConsistencyResult {
  std::vector<ConsistencyIssue> issues;

  [[nodiscard]] static ConsistencyResult success() { return ConsistencyResult{}; }

  [[nodiscard]] static ConsistencyResult failure(ConsistencyIssueCode code, std::string message) {
    ConsistencyResult result;
    result.issues.push_back(ConsistencyIssue{code, std::move(message)});
    return result;
  }

  [[nodiscard]] bool is_consistent() const noexcept { return issues.empty(); }
};

namespace detail {

template <typename ValueT>
using EventIdT = typename ExecutionGraphT<ValueT>::EventId;

template <typename EventId>
struct NoMissingReadsToleranceT {
  [[nodiscard]] constexpr bool operator()(EventId) const noexcept { return false; }
};

template <typename ValueT>
struct ValidationPassResultT {
  ConsistencyResult result{};
  std::vector<std::pair<EventIdT<ValueT>, EventIdT<ValueT>>> valid_rf_edges{};
  bool cycle_query_safe{true};
};

inline void add_issue(ConsistencyResult& result, ConsistencyIssueCode code, std::string message) {
  result.issues.push_back(ConsistencyIssue{code, std::move(message)});
}

inline void add_causal_cycle_issue(ConsistencyResult& result) {
  add_issue(result, ConsistencyIssueCode::CausalCycle,
            "program order and reads-from relations form a causal cycle");
}

template <typename ValueT, bool CollectValidRfEdges, typename MissingReadsToleranceT>
[[nodiscard]] ValidationPassResultT<ValueT> validate_graph(
    const ExecutionGraphT<ValueT>& graph, const MissingReadsToleranceT& missing_reads_tolerance) {
  using EventId = typename ExecutionGraphT<ValueT>::EventId;

  ValidationPassResultT<ValueT> validation;

  const auto event_count = graph.events().size();
  std::vector<bool> receive_has_source(event_count, false);
  std::unordered_map<EventId, std::size_t> send_read_count;
  send_read_count.reserve(graph.reads_from().size());
  if constexpr (CollectValidRfEdges) {
    validation.valid_rf_edges.reserve(graph.reads_from().size());
  }

  for (const auto& [receive_id, source] : graph.reads_from()) {
    bool has_valid_ids = true;

    if (!graph.is_valid_event_id(receive_id)) {
      add_issue(validation.result, ConsistencyIssueCode::InvalidEventReference,
                "reads-from target references unknown event id " + std::to_string(receive_id));
      validation.cycle_query_safe = false;
      has_valid_ids = false;
    }
    if (source.is_send()) {
      const auto source_id = source.send_id();
      if (!graph.is_valid_event_id(source_id)) {
        add_issue(validation.result, ConsistencyIssueCode::InvalidEventReference,
                  "reads-from source references unknown event id " + std::to_string(source_id));
        validation.cycle_query_safe = false;
        has_valid_ids = false;
      }
    }

    if (!has_valid_ids) {
      continue;
    }

    receive_has_source[receive_id] = true;

    const auto& receive_event = graph.event(receive_id);
    bool has_valid_endpoint_kinds = true;
    if (!is_receive(receive_event)) {
      add_issue(validation.result, ConsistencyIssueCode::ReadsFromTargetNotReceive,
                "reads-from target event " + std::to_string(receive_id) + " is not a receive");
      validation.cycle_query_safe = false;
      has_valid_endpoint_kinds = false;
    }
    if (source.is_send()) {
      const auto source_id = source.send_id();
      const auto& source_event = graph.event(source_id);
      if (!is_send(source_event)) {
        add_issue(validation.result, ConsistencyIssueCode::ReadsFromSourceNotSend,
                  "reads-from source event " + std::to_string(source_id) + " is not a send");
        validation.cycle_query_safe = false;
        has_valid_endpoint_kinds = false;
      }
    }

    if (!has_valid_endpoint_kinds) {
      continue;
    }

    const auto* receive_label = as_receive(receive_event);
    if (receive_label == nullptr) {
      continue;
    }

    if (source.is_bottom()) {
      if (receive_label->is_blocking()) {
        add_issue(validation.result, ConsistencyIssueCode::BlockingReceiveReadsBottom,
                  "blocking receive event " + std::to_string(receive_id) + " reads from bottom");
      }
      continue;
    }

    const auto source_id = source.send_id();
    const auto& source_event = graph.event(source_id);
    const auto* send_label = as_send(source_event);
    if (send_label == nullptr) {
      continue;
    }

    auto& read_count = send_read_count[source_id];
    ++read_count;
    if (read_count > 1U) {
      add_issue(validation.result, ConsistencyIssueCode::SendConsumedMultipleTimes,
                "send event " + std::to_string(source_id) +
                    " is consumed by more than one receive");
    }

    if (send_label->destination != receive_event.thread) {
      add_issue(validation.result, ConsistencyIssueCode::ReceiveDestinationMismatch,
                "receive event " + std::to_string(receive_id) + " in thread " +
                    std::to_string(receive_event.thread) + " reads from send event " +
                    std::to_string(source_id) + " targeting thread " +
                    std::to_string(send_label->destination));
    }

    if (!receive_label->accepts(send_label->value)) {
      add_issue(validation.result, ConsistencyIssueCode::ReceiveValueMismatch,
                "receive event " + std::to_string(receive_id) +
                    " does not accept the value sent by event " + std::to_string(source_id));
    }

    if constexpr (CollectValidRfEdges) {
      validation.valid_rf_edges.emplace_back(source_id, receive_id);
    }
  }

  for (EventId event_id = 0; event_id < event_count; ++event_id) {
    if (!is_receive(graph.event(event_id)) || receive_has_source[event_id] ||
        missing_reads_tolerance(event_id)) {
      continue;
    }
    add_issue(validation.result, ConsistencyIssueCode::MissingReadsFromForReceive,
              "receive event " + std::to_string(event_id) + " has no reads-from assignment");
  }

  return validation;
}

template <typename ValueT>
[[nodiscard]] inline ValidationPassResultT<ValueT> validate_graph(
    const ExecutionGraphT<ValueT>& graph) {
  return validate_graph<ValueT, true>(graph, NoMissingReadsToleranceT<EventIdT<ValueT>>{});
}

template <typename ValueT>
[[nodiscard]] bool has_causal_cycle(
    const ProgramOrderRelation& po,
    const std::vector<std::pair<EventIdT<ValueT>, EventIdT<ValueT>>>& rf_edges,
    const std::size_t event_count) {
  using EventId = EventIdT<ValueT>;

  std::vector<std::vector<EventId>> successors(event_count);

  for (EventId from = 0; from < event_count; ++from) {
    po.for_each_successor(from, [&](const NodeId to) { successors[from].push_back(to); });
  }

  for (const auto& [from, to] : rf_edges) {
    if (from < event_count && to < event_count) {
      successors[from].push_back(to);
    }
  }

  std::vector<std::uint8_t> visit_state(event_count, 0U);
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

  for (EventId event_id = 0; event_id < event_count; ++event_id) {
    if (visit_state[event_id] == 0U && dfs(dfs, event_id)) {
      return true;
    }
  }

  return false;
}

template <typename ValueT, typename MissingReadsToleranceT>
[[nodiscard]] inline ConsistencyResult check_async_execution_graph(
    const ExecutionGraphT<ValueT>& graph, const MissingReadsToleranceT& missing_reads_tolerance) {
  auto validation = validate_graph<ValueT, true>(graph, missing_reads_tolerance);
  if (has_causal_cycle<ValueT>(graph.po_relation(), validation.valid_rf_edges,
                               graph.events().size())) {
    add_causal_cycle_issue(validation.result);
  }
  return validation.result;
}

template <typename ValueT, typename MissingReadsToleranceT>
[[nodiscard]] inline ConsistencyResult check_async_exploration_graph(
    ExplorationGraphT<ValueT>& graph, const MissingReadsToleranceT& missing_reads_tolerance) {
  auto validation = validate_graph<ValueT, false>(graph.execution_graph(), missing_reads_tolerance);
  if (!validation.cycle_query_safe) {
    return check_async_execution_graph(graph.execution_graph(), missing_reads_tolerance);
  }

  if (!graph.is_known_acyclic()) {
    const bool has_cycle =
        graph.has_porf_cache() ? graph.has_causal_cycle() : graph.has_causal_cycle_without_cache();
    if (has_cycle) {
      add_causal_cycle_issue(validation.result);
      return validation.result;
    }
  }

  if (validation.result.is_consistent()) {
    graph.mark_known_acyclic();
  }
  return validation.result;
}

template <typename ValueT>
[[nodiscard]] inline std::map<std::pair<ThreadId, ThreadId>, std::vector<EventIdT<ValueT>>>
group_fifo_sends(const ExecutionGraphT<ValueT>& graph) {
  using EventId = EventIdT<ValueT>;

  std::map<std::pair<ThreadId, ThreadId>, std::vector<EventId>> grouped;
  for (const auto send_id : graph.send_event_ids()) {
    const auto* send = as_send(graph.event(send_id));
    if (send == nullptr) {
      continue;
    }
    grouped[{graph.event(send_id).thread, send->destination}].push_back(send_id);
  }

  for (auto& [_, send_ids] : grouped) {
    std::sort(send_ids.begin(), send_ids.end(), [&](const EventId lhs, const EventId rhs) {
      const auto& lhs_event = graph.event(lhs);
      const auto& rhs_event = graph.event(rhs);
      if (lhs_event.index != rhs_event.index) {
        return lhs_event.index < rhs_event.index;
      }
      return lhs < rhs;
    });
  }
  return grouped;
}

template <typename ValueT>
[[nodiscard]] inline std::vector<std::optional<EventIdT<ValueT>>> compute_send_consumers(
    const ExecutionGraphT<ValueT>& graph) {
  using EventId = EventIdT<ValueT>;

  std::vector<std::optional<EventId>> consumers(graph.events().size(), std::nullopt);
  for (const auto& [receive_id, source] : graph.reads_from()) {
    if (!source.is_send() || !graph.is_valid_event_id(receive_id) ||
        !graph.is_valid_event_id(source.send_id())) {
      continue;
    }
    const auto& receive_event = graph.event(receive_id);
    const auto& send_event = graph.event(source.send_id());
    const auto* receive = as_receive(receive_event);
    const auto* send = as_send(send_event);
    if (receive == nullptr || send == nullptr) {
      continue;
    }
    if (send->destination != receive_event.thread || !receive->accepts(send->value)) {
      continue;
    }
    consumers[source.send_id()] = receive_id;
  }
  return consumers;
}

template <typename ValueT>
inline void add_fifo_p2p_clause_b_issues(const ExecutionGraphT<ValueT>& graph,
                                         const std::map<std::pair<ThreadId, ThreadId>,
                                                        std::vector<EventIdT<ValueT>>>& fifo_groups,
                                         const std::vector<std::optional<EventIdT<ValueT>>>& send_consumers,
                                         ConsistencyResult& result) {
  using EventId = EventIdT<ValueT>;

  for (const auto& [_, send_ids] : fifo_groups) {
    for (std::size_t later_index = 0; later_index < send_ids.size(); ++later_index) {
      const EventId later_send_id = send_ids[later_index];
      const auto consumer = send_consumers[later_send_id];
      if (!consumer.has_value()) {
        continue;
      }

      const auto* receive = as_receive(graph.event(*consumer));
      if (receive == nullptr) {
        continue;
      }

      for (std::size_t earlier_index = 0; earlier_index < later_index; ++earlier_index) {
        const EventId earlier_send_id = send_ids[earlier_index];
        if (send_consumers[earlier_send_id].has_value()) {
          continue;
        }

        const auto* earlier_send = as_send(graph.event(earlier_send_id));
        if (earlier_send == nullptr || !receive->accepts(earlier_send->value)) {
          continue;
        }

        add_issue(result, ConsistencyIssueCode::FifoP2PUnreadEarlierMatchingSend,
                  "receive event " + std::to_string(*consumer) + " reads from send event " +
                      std::to_string(later_send_id) +
                      " while unread earlier matching send event " +
                      std::to_string(earlier_send_id) + " remains available");
        break;
      }
    }
  }
}

template <typename ValueT>
inline void add_fifo_p2p_clause_c_issues(const ExecutionGraphT<ValueT>& graph,
                                         const std::map<std::pair<ThreadId, ThreadId>,
                                                        std::vector<EventIdT<ValueT>>>& fifo_groups,
                                         const std::vector<std::optional<EventIdT<ValueT>>>& send_consumers,
                                         ConsistencyResult& result) {
  using EventId = EventIdT<ValueT>;

  for (const auto& [_, send_ids] : fifo_groups) {
    for (std::size_t later_index = 0; later_index < send_ids.size(); ++later_index) {
      const EventId later_send_id = send_ids[later_index];
      const auto earlier_receive_id = send_consumers[later_send_id];
      if (!earlier_receive_id.has_value()) {
        continue;
      }

      const auto& earlier_receive_event = graph.event(*earlier_receive_id);
      const auto* earlier_receive = as_receive(earlier_receive_event);
      if (earlier_receive == nullptr) {
        continue;
      }

      for (std::size_t earlier_index = 0; earlier_index < later_index; ++earlier_index) {
        const EventId earlier_send_id = send_ids[earlier_index];
        const auto later_receive_id = send_consumers[earlier_send_id];
        if (!later_receive_id.has_value()) {
          continue;
        }

        const auto& later_receive_event = graph.event(*later_receive_id);
        if (later_receive_event.thread != earlier_receive_event.thread ||
            later_receive_event.index <= earlier_receive_event.index) {
          continue;
        }

        const auto* earlier_send = as_send(graph.event(earlier_send_id));
        if (earlier_send == nullptr || !earlier_receive->accepts(earlier_send->value)) {
          continue;
        }

        add_issue(result, ConsistencyIssueCode::FifoP2PReceiveOrderViolation,
                  "receive event " + std::to_string(*earlier_receive_id) +
                      " consumes later send event " + std::to_string(later_send_id) +
                      " before receive event " + std::to_string(*later_receive_id) +
                      " consumes earlier matching send event " +
                      std::to_string(earlier_send_id));
        break;
      }
    }
  }
}

template <typename ValueT>
inline void add_fifo_p2p_issues(const ExecutionGraphT<ValueT>& graph, ConsistencyResult& result) {
  const auto fifo_groups = group_fifo_sends(graph);
  const auto send_consumers = compute_send_consumers(graph);
  add_fifo_p2p_clause_b_issues(graph, fifo_groups, send_consumers, result);
  add_fifo_p2p_clause_c_issues(graph, fifo_groups, send_consumers, result);
}

template <typename ValueT, typename MissingReadsToleranceT>
[[nodiscard]] inline ConsistencyResult check_execution_graph(
    const ExecutionGraphT<ValueT>& graph, const CommunicationModel communication_model,
    const MissingReadsToleranceT& missing_reads_tolerance) {
  auto result = check_async_execution_graph(graph, missing_reads_tolerance);
  if (communication_model == CommunicationModel::FifoP2P && result.is_consistent()) {
    add_fifo_p2p_issues(graph, result);
  }
  return result;
}

template <typename ValueT, typename MissingReadsToleranceT>
[[nodiscard]] inline ConsistencyResult check_exploration_graph(
    ExplorationGraphT<ValueT>& graph, const CommunicationModel communication_model,
    const MissingReadsToleranceT& missing_reads_tolerance) {
  auto result = check_async_exploration_graph(graph, missing_reads_tolerance);
  if (communication_model == CommunicationModel::FifoP2P && result.is_consistent()) {
    add_fifo_p2p_issues(graph.execution_graph(), result);
  }
  return result;
}

}  // namespace detail

template <typename ValueT>
class AsyncConsistencyCheckerT {
 public:
  [[nodiscard]] ConsistencyResult check(const ExecutionGraphT<ValueT>& graph) const {
    return detail::check_execution_graph(graph, CommunicationModel::Async,
                                         detail::NoMissingReadsToleranceT<
                                             typename ExecutionGraphT<ValueT>::EventId>{});
  }

  [[nodiscard]] ConsistencyResult check(ExplorationGraphT<ValueT>& graph) const {
    return detail::check_exploration_graph(graph, CommunicationModel::Async,
                                           detail::NoMissingReadsToleranceT<
                                               typename ExecutionGraphT<ValueT>::EventId>{});
  }
};

template <typename ValueT>
class FifoP2PConsistencyCheckerT {
 public:
  [[nodiscard]] ConsistencyResult check(const ExecutionGraphT<ValueT>& graph) const {
    return detail::check_execution_graph(graph, CommunicationModel::FifoP2P,
                                         detail::NoMissingReadsToleranceT<
                                             typename ExecutionGraphT<ValueT>::EventId>{});
  }

  [[nodiscard]] ConsistencyResult check(ExplorationGraphT<ValueT>& graph) const {
    return detail::check_exploration_graph(graph, CommunicationModel::FifoP2P,
                                           detail::NoMissingReadsToleranceT<
                                               typename ExecutionGraphT<ValueT>::EventId>{});
  }
};

template <typename ValueT>
class ConsistencyCheckerT {
 public:
  explicit ConsistencyCheckerT(
      const CommunicationModel communication_model = CommunicationModel::Async)
      : communication_model_(communication_model) {}

  [[nodiscard]] ConsistencyResult check(const ExecutionGraphT<ValueT>& graph) const {
    return detail::check_execution_graph(graph, communication_model_,
                                         detail::NoMissingReadsToleranceT<
                                             typename ExecutionGraphT<ValueT>::EventId>{});
  }

  [[nodiscard]] ConsistencyResult check(ExplorationGraphT<ValueT>& graph) const {
    return detail::check_exploration_graph(graph, communication_model_,
                                           detail::NoMissingReadsToleranceT<
                                               typename ExecutionGraphT<ValueT>::EventId>{});
  }

 private:
  CommunicationModel communication_model_{CommunicationModel::Async};
};

using AsyncConsistencyChecker = AsyncConsistencyCheckerT<Value>;
using FifoP2PConsistencyChecker = FifoP2PConsistencyCheckerT<Value>;
using ConsistencyChecker = ConsistencyCheckerT<Value>;

}  // namespace dpor::model
