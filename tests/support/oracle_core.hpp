#pragma once

#include "dpor/algo/dpor.hpp"
#include "dpor/model/consistency.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dpor::test_support {

template <typename ValueT>
struct OracleExplorationStatsT {
  std::set<std::string> signatures{};
  std::size_t paths_explored{0};
};

template <typename ValueT>
[[nodiscard]] inline std::string event_signature(const model::EventT<ValueT>& event) {
  std::ostringstream oss;
  oss << "t" << event.thread << ":i" << event.index << ":";
  if (const auto* send = model::as_send(event)) {
    oss << "S(dst=" << send->destination << ",v=" << send->value << ")";
  } else if (const auto* nd = model::as_nondeterministic_choice(event)) {
    oss << "ND(v=" << nd->value << ")";
  } else if (const auto* recv = model::as_receive(event)) {
    oss << (recv->is_nonblocking() ? "Rnb" : "Rb");
  } else if (model::is_block(event)) {
    oss << "B";
  } else if (model::is_error(event)) {
    oss << "E";
  }
  return oss.str();
}

template <typename ValueT>
[[nodiscard]] inline std::string graph_signature(const model::ExplorationGraphT<ValueT>& graph) {
  std::vector<std::string> events;
  events.reserve(graph.events().size());
  for (const auto& event : graph.events()) {
    events.push_back(event_signature(event));
  }
  std::sort(events.begin(), events.end());

  std::vector<std::string> rf_edges;
  rf_edges.reserve(graph.reads_from().size());
  for (const auto& [recv_id, source] : graph.reads_from()) {
    if (source.is_bottom()) {
      rf_edges.push_back("BOTTOM->" + event_signature(graph.event(recv_id)));
      continue;
    }
    rf_edges.push_back(
        event_signature(graph.event(source.send_id())) + "->" +
        event_signature(graph.event(recv_id)));
  }
  std::sort(rf_edges.begin(), rf_edges.end());

  std::ostringstream oss;
  for (const auto& event : events) {
    oss << event << ";";
  }
  oss << "|";
  for (const auto& edge : rf_edges) {
    oss << edge << ";";
  }
  return oss.str();
}

template <typename ValueT>
struct OracleTransitionT {
  model::ThreadId thread{};
  model::EventLabelT<ValueT> label{};
  std::optional<typename model::ExplorationGraphT<ValueT>::ReadsFromSource> rf_source{};
};

template <typename ValueT>
[[nodiscard]] inline std::vector<model::ThreadId> sorted_thread_ids(
    const algo::ProgramT<ValueT>& program) {
  program.threads.validate_compact_thread_ids();
  std::vector<model::ThreadId> thread_ids;
  thread_ids.reserve(program.threads.size());
  program.threads.for_each_assigned([&](const model::ThreadId tid, const auto&) {
    thread_ids.push_back(tid);
  });
  return thread_ids;
}

template <typename ValueT>
[[nodiscard]] inline bool has_compatible_unread_send(
    const model::ExplorationGraphT<ValueT>& graph,
    const model::ThreadId tid,
    const model::ReceiveLabelT<ValueT>& recv) {
  for (const auto send_id : graph.unread_send_event_ids()) {
    const auto* send = model::as_send(graph.event(send_id));
    if (send != nullptr &&
        send->destination == tid &&
        recv.accepts(send->value)) {
      return true;
    }
  }
  return false;
}

template <typename ValueT>
[[nodiscard]] inline typename model::ExplorationGraphT<ValueT>::EventId
find_last_event_in_thread(
    const model::ExplorationGraphT<ValueT>& graph,
    const model::ThreadId tid) {
  using EventId = typename model::ExplorationGraphT<ValueT>::EventId;

  EventId last_id = model::ExplorationGraphT<ValueT>::kNoSource;
  model::EventIndex last_index = 0;
  for (EventId id = 0; id < graph.event_count(); ++id) {
    const auto& evt = graph.event(id);
    if (evt.thread == tid &&
        (last_id == model::ExplorationGraphT<ValueT>::kNoSource || evt.index > last_index)) {
      last_id = id;
      last_index = evt.index;
    }
  }
  return last_id;
}

template <typename ValueT>
[[nodiscard]] inline std::vector<OracleTransitionT<ValueT>> enumerate_enabled_transitions(
    const algo::ProgramT<ValueT>& program,
    const model::ExplorationGraphT<ValueT>& graph) {
  using OracleTransition = OracleTransitionT<ValueT>;

  const auto thread_ids = sorted_thread_ids(program);
  std::vector<OracleTransition> transitions;

  for (const auto tid : thread_ids) {
    if (graph.thread_is_terminated(tid)) {
      continue;
    }

    const auto& thread_fn = program.threads.at(tid);
    const auto trace = graph.thread_trace(tid);
    const auto step = graph.thread_event_count(tid);
    const auto next_label = thread_fn(trace, step);
    if (!next_label.has_value()) {
      continue;
    }

    if (std::holds_alternative<model::BlockLabel>(*next_label)) {
      throw std::logic_error(
          "stress program returned BlockLabel; blocks are internal to DPOR");
    }

    if (const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&*next_label)) {
      bool found_compatible = false;
      for (const auto send_id : graph.unread_send_event_ids()) {
        const auto* send = model::as_send(graph.event(send_id));
        if (send == nullptr ||
            send->destination != tid ||
            !recv->accepts(send->value)) {
          continue;
        }
        found_compatible = true;
        transitions.push_back(OracleTransition{
            .thread = tid,
            .label = *next_label,
            .rf_source = model::ExplorationGraphT<ValueT>::ReadsFromSource::from_send(send_id),
        });
      }
      if (recv->is_nonblocking()) {
        transitions.push_back(OracleTransition{
            .thread = tid,
            .label = *next_label,
            .rf_source = model::ExplorationGraphT<ValueT>::ReadsFromSource::bottom(),
        });
      } else if (!found_compatible) {
        transitions.push_back(OracleTransition{
            .thread = tid,
            .label = model::EventLabelT<ValueT>{model::BlockLabel{}},
            .rf_source = std::nullopt,
        });
      }
      continue;
    }

    if (const auto* nd =
            std::get_if<model::NondeterministicChoiceLabelT<ValueT>>(&*next_label)) {
      if (nd->choices.empty()) {
        transitions.push_back(OracleTransition{
            .thread = tid,
            .label = *next_label,
            .rf_source = std::nullopt,
        });
        continue;
      }

      for (const auto& choice : nd->choices) {
        auto label = *nd;
        label.value = choice;
        transitions.push_back(OracleTransition{
            .thread = tid,
            .label = model::EventLabelT<ValueT>{label},
            .rf_source = std::nullopt,
        });
      }
      continue;
    }

    transitions.push_back(OracleTransition{
        .thread = tid,
        .label = *next_label,
        .rf_source = std::nullopt,
    });
  }

  return transitions;
}

template <typename ValueT>
[[nodiscard]] inline std::vector<model::ExplorationGraphT<ValueT>> enumerate_reschedulable_graphs(
    const algo::ProgramT<ValueT>& program,
    const model::ExplorationGraphT<ValueT>& graph) {
  using ExplorationGraph = model::ExplorationGraphT<ValueT>;

  std::vector<ExplorationGraph> rescheduled;
  const auto thread_ids = sorted_thread_ids(program);

  for (const auto tid : thread_ids) {
    const auto last_id = find_last_event_in_thread(graph, tid);
    if (last_id == ExplorationGraph::kNoSource ||
        !model::is_block(graph.event(last_id))) {
      continue;
    }

    std::unordered_set<typename ExplorationGraph::EventId> keep_set;
    keep_set.reserve(graph.event_count());
    for (typename ExplorationGraph::EventId id = 0; id < graph.event_count(); ++id) {
      if (id != last_id) {
        keep_set.insert(id);
      }
    }
    auto unblocked = graph.restrict(keep_set);

    const auto& thread_fn = program.threads.at(tid);
    const auto trace = unblocked.thread_trace(tid);
    const auto step = unblocked.thread_event_count(tid);
    const auto next_label = thread_fn(trace, step);
    if (!next_label.has_value()) {
      throw std::logic_error(
          "rescheduling failed in oracle: blocked thread became done");
    }
    if (std::holds_alternative<model::BlockLabel>(*next_label)) {
      throw std::logic_error(
          "stress program returned BlockLabel; blocks are internal to DPOR");
    }
    const auto* recv = std::get_if<model::ReceiveLabelT<ValueT>>(&*next_label);
    if (recv == nullptr) {
      throw std::logic_error(
          "rescheduling failed in oracle: blocked thread is not waiting on receive");
    }
    if (recv->is_nonblocking()) {
      throw std::logic_error(
          "rescheduling failed in oracle: blocked thread became non-blocking receive");
    }

    if (has_compatible_unread_send(unblocked, tid, *recv)) {
      rescheduled.push_back(std::move(unblocked));
    }
  }

  return rescheduled;
}

template <typename ValueT>
inline void enumerate_consistent_executions(
    const algo::ProgramT<ValueT>& program,
    const model::ExplorationGraphT<ValueT>& graph,
    model::AsyncConsistencyCheckerT<ValueT>& checker,
    OracleExplorationStatsT<ValueT>& stats) {
  const auto transitions = enumerate_enabled_transitions(program, graph);
  if (!transitions.empty()) {
    for (const auto& transition : transitions) {
      auto next_graph = graph;
      const auto event_id = next_graph.add_event(transition.thread, transition.label);
      if (transition.rf_source.has_value()) {
        if (transition.rf_source->is_bottom()) {
          next_graph.set_reads_from_bottom(event_id);
        } else {
          next_graph.set_reads_from(event_id, transition.rf_source->send_id());
        }
      }

      const auto consistency = checker.check(next_graph.execution_graph());
      if (!consistency.is_consistent()) {
        continue;
      }

      enumerate_consistent_executions(program, next_graph, checker, stats);
    }
    return;
  }

  const auto rescheduled = enumerate_reschedulable_graphs(program, graph);
  if (!rescheduled.empty()) {
    for (const auto& unblocked : rescheduled) {
      enumerate_consistent_executions(program, unblocked, checker, stats);
    }
    return;
  }

  ++stats.paths_explored;
  stats.signatures.insert(graph_signature(graph));
}

template <typename ValueT>
[[nodiscard]] inline OracleExplorationStatsT<ValueT> collect_oracle_stats(
    const algo::ProgramT<ValueT>& program) {
  model::AsyncConsistencyCheckerT<ValueT> checker;
  OracleExplorationStatsT<ValueT> stats;
  enumerate_consistent_executions(program, model::ExplorationGraphT<ValueT>{}, checker, stats);
  return stats;
}

template <typename ValueT>
[[nodiscard]] inline std::set<std::string> collect_oracle_signatures(
    const algo::ProgramT<ValueT>& program) {
  return collect_oracle_stats(program).signatures;
}

template <typename ValueT>
struct OracleComparisonT {
  algo::VerifyResult result{};
  std::set<std::string> oracle_signatures{};
  std::vector<std::string> dpor_observed{};
  std::set<std::string> dpor_unique{};
  std::set<std::string> missing_from_dpor{};
  std::set<std::string> unexpected_in_dpor{};
  bool found_inconsistent_graph{false};
};

template <typename ValueT>
[[nodiscard]] inline OracleComparisonT<ValueT> compare_dpor_with_oracle(
    const algo::ProgramT<ValueT>& program) {
  OracleComparisonT<ValueT> comparison;
  comparison.oracle_signatures = collect_oracle_stats(program).signatures;

  model::AsyncConsistencyCheckerT<ValueT> checker;
  algo::DporConfigT<ValueT> config;
  config.program = program;
  config.on_execution = [&](const model::ExplorationGraphT<ValueT>& graph) {
    const auto consistency = checker.check(graph.execution_graph());
    if (!consistency.is_consistent()) {
      comparison.found_inconsistent_graph = true;
    }
    const auto sig = graph_signature(graph);
    comparison.dpor_observed.push_back(sig);
    comparison.dpor_unique.insert(sig);
  };

  comparison.result = algo::verify(config);

  std::set_difference(
      comparison.oracle_signatures.begin(),
      comparison.oracle_signatures.end(),
      comparison.dpor_unique.begin(),
      comparison.dpor_unique.end(),
      std::inserter(comparison.missing_from_dpor, comparison.missing_from_dpor.end()));
  std::set_difference(
      comparison.dpor_unique.begin(),
      comparison.dpor_unique.end(),
      comparison.oracle_signatures.begin(),
      comparison.oracle_signatures.end(),
      std::inserter(comparison.unexpected_in_dpor, comparison.unexpected_in_dpor.end()));

  return comparison;
}

}  // namespace dpor::test_support
