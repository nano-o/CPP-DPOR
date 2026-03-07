#pragma once

#include "dpor/model/exploration_graph.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dpor::model {

enum class ConsistencyIssueCode {
  InvalidEventReference,
  ReadsFromTargetNotReceive,
  ReadsFromSourceNotSend,
  MissingReadsFromForReceive,
  BlockingReceiveReadsBottom,
  SendConsumedMultipleTimes,
  ReceiveDestinationMismatch,
  ReceiveValueMismatch,
  CausalCycle,
};

struct ConsistencyIssue {
  ConsistencyIssueCode code{};
  std::string message{};
};

struct ConsistencyResult {
  std::vector<ConsistencyIssue> issues{};

  [[nodiscard]] static ConsistencyResult success() {
    return ConsistencyResult{};
  }

  [[nodiscard]] static ConsistencyResult failure(ConsistencyIssueCode code, std::string message) {
    ConsistencyResult result;
    result.issues.push_back(ConsistencyIssue{code, std::move(message)});
    return result;
  }

  [[nodiscard]] bool is_consistent() const noexcept {
    return issues.empty();
  }
};

template <typename ValueT>
class AsyncConsistencyCheckerT {
 public:
  [[nodiscard]] ConsistencyResult check(const ExecutionGraphT<ValueT>& graph) const {
    auto validation = validate_graph<true>(graph);
    if (has_causal_cycle(
            graph.po_relation(),
            validation.valid_rf_edges,
            graph.events().size())) {
      add_causal_cycle_issue(validation.result);
    }
    return validation.result;
  }

  [[nodiscard]] ConsistencyResult check(const ExplorationGraphT<ValueT>& graph) const {
    auto validation = validate_graph<false>(graph.execution_graph());
    if (!validation.cycle_query_safe) {
      return validation.result;
    }

    const bool has_cycle = graph.has_porf_cache()
        ? graph.has_causal_cycle()
        : graph.has_causal_cycle_without_cache();
    if (has_cycle) {
      add_causal_cycle_issue(validation.result);
    }
    return validation.result;
  }

 private:
  using EventId = typename ExecutionGraphT<ValueT>::EventId;

  struct ValidationPassResult {
    ConsistencyResult result{};
    std::vector<std::pair<EventId, EventId>> valid_rf_edges{};
    bool cycle_query_safe{true};
  };

  static void add_issue(ConsistencyResult& result, ConsistencyIssueCode code, std::string message) {
    result.issues.push_back(ConsistencyIssue{code, std::move(message)});
  }

  static void add_causal_cycle_issue(ConsistencyResult& result) {
    add_issue(
        result,
        ConsistencyIssueCode::CausalCycle,
        "program order and reads-from relations form a causal cycle");
  }

  template <bool CollectValidRfEdges>
  [[nodiscard]] static ValidationPassResult validate_graph(
      const ExecutionGraphT<ValueT>& graph) {
    ValidationPassResult validation;

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
        add_issue(
            validation.result,
            ConsistencyIssueCode::InvalidEventReference,
            "reads-from target references unknown event id " + std::to_string(receive_id));
        validation.cycle_query_safe = false;
        has_valid_ids = false;
      }
      if (source.is_send()) {
        const auto source_id = source.send_id();
        if (!graph.is_valid_event_id(source_id)) {
          add_issue(
              validation.result,
              ConsistencyIssueCode::InvalidEventReference,
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
        add_issue(
            validation.result,
            ConsistencyIssueCode::ReadsFromTargetNotReceive,
            "reads-from target event " + std::to_string(receive_id) + " is not a receive");
        validation.cycle_query_safe = false;
        has_valid_endpoint_kinds = false;
      }
      if (source.is_send()) {
        const auto source_id = source.send_id();
        const auto& source_event = graph.event(source_id);
        if (!is_send(source_event)) {
          add_issue(
              validation.result,
              ConsistencyIssueCode::ReadsFromSourceNotSend,
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
          add_issue(
              validation.result,
              ConsistencyIssueCode::BlockingReceiveReadsBottom,
              "blocking receive event " + std::to_string(receive_id) +
                  " reads from bottom");
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
        add_issue(
            validation.result,
            ConsistencyIssueCode::SendConsumedMultipleTimes,
            "send event " + std::to_string(source_id) + " is consumed by more than one receive");
      }

      if (send_label->destination != receive_event.thread) {
        add_issue(
            validation.result,
            ConsistencyIssueCode::ReceiveDestinationMismatch,
            "receive event " + std::to_string(receive_id) + " in thread " +
                std::to_string(receive_event.thread) + " reads from send event " +
                std::to_string(source_id) + " targeting thread " +
                std::to_string(send_label->destination));
      }

      if (!receive_label->accepts(send_label->value)) {
        add_issue(
            validation.result,
            ConsistencyIssueCode::ReceiveValueMismatch,
            "receive event " + std::to_string(receive_id) +
                " does not accept the value sent by event " + std::to_string(source_id));
      }

      if constexpr (CollectValidRfEdges) {
        validation.valid_rf_edges.emplace_back(source_id, receive_id);
      }
    }

    for (EventId event_id = 0; event_id < event_count; ++event_id) {
      if (is_receive(graph.event(event_id)) && !receive_has_source[event_id]) {
        add_issue(
            validation.result,
            ConsistencyIssueCode::MissingReadsFromForReceive,
            "receive event " + std::to_string(event_id) +
                " has no reads-from assignment");
      }
    }

    return validation;
  }

  [[nodiscard]] static bool has_causal_cycle(
      const ProgramOrderRelation& po,
      const std::vector<std::pair<EventId, EventId>>& rf_edges,
      const std::size_t event_count) {
    std::vector<std::vector<EventId>> successors(event_count);

    for (EventId from = 0; from < event_count; ++from) {
      po.for_each_successor(from, [&](const NodeId to) {
        successors[from].push_back(to);
      });
    }

    for (const auto& [from, to] : rf_edges) {
      if (from < event_count && to < event_count) {
        successors[from].push_back(to);
      }
    }

    // 0 = unvisited, 1 = visiting (on stack), 2 = visited.
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
};

using AsyncConsistencyChecker = AsyncConsistencyCheckerT<Value>;

}  // namespace dpor::model
