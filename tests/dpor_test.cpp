#include "dpor/algo/dpor.hpp"

#include "dpor/model/consistency.hpp"

#include "support/oracle.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dpor::algo;
using namespace dpor::model;
using dpor::test_support::require_dpor_matches_oracle;

std::string event_signature(const Event& event) {
  std::ostringstream oss;
  oss << "t" << event.thread << ":i" << event.index << ":";
  if (const auto* send = as_send(event)) {
    oss << "S(dst=" << send->destination << ",v=" << send->value << ")";
  } else if (const auto* nd = as_nondeterministic_choice(event)) {
    oss << "ND(v=" << nd->value << ")";
  } else if (const auto* recv = as_receive(event)) {
    oss << (recv->is_nonblocking() ? "Rnb" : "Rb");
  } else if (is_block(event)) {
    oss << "B";
  } else if (is_error(event)) {
    oss << "E";
  }
  return oss.str();
}

std::string graph_signature(const ExplorationGraph& graph) {
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
    rf_edges.push_back(event_signature(graph.event(source.send_id())) + "->" +
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

ExplorationGraph::EventId find_event_id_by_thread_index(const ExplorationGraph& graph,
                                                        const ThreadId thread,
                                                        const EventIndex index) {
  for (ExplorationGraph::EventId id = 0; id < graph.event_count(); ++id) {
    const auto& event = graph.event(id);
    if (event.thread == thread && event.index == index) {
      return id;
    }
  }
  return ExplorationGraph::kNoSource;
}

struct ObservedRun {
  VerifyResult result{};
  std::vector<std::string> observed;
  std::set<std::string> unique;
};

template <typename RunFn>
ObservedRun collect_observed_executions(const Program& program, const RunFn& run) {
  ObservedRun observed_run;
  std::mutex observed_mutex;

  DporConfig config;
  config.program = program;
  config.on_terminal_execution = [&](const ExplorationGraph& graph) {
    const auto signature = graph_signature(graph);
    std::lock_guard lock(observed_mutex);
    observed_run.observed.push_back(signature);
    observed_run.unique.insert(signature);
  };

  observed_run.result = run(config);
  return observed_run;
}

Program make_parallel_mixed_program() {
  Program program;

  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"x"}, ReceiveMode::NonBlocking);
    }
    if (step == 1 && trace.size() == 1) {
      return SendLabel{
          .destination = 3,
          .value = trace[0].is_bottom() ? "timeout" : trace[0].value(),
      };
    }
    return std::nullopt;
  };

  program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "x"};
    }
    return std::nullopt;
  };

  program.threads[3] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"timeout", "x"});
    }
    if (step == 1 && trace.size() == 1) {
      return NondeterministicChoiceLabel{
          .value = "ack",
          .choices = {"ack", "nack"},
      };
    }
    return std::nullopt;
  };

  return program;
}

struct RejectingEnqueueExecutor {
  template <typename ValueT>
  [[nodiscard]] bool try_enqueue(
      dpor::algo::detail::ExplorationTask<ValueT>& /*task*/) const noexcept {
    return false;
  }
};
}  // namespace

// --- Empty and trivial programs ---

TEST_CASE("empty program explores 1 execution", "[algo][dpor]") {
  DporConfig config;
  config.program.threads = {};

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  require_dpor_matches_oracle(config.program, "empty program");
}

TEST_CASE("verify rejects sparse program thread ids", "[algo][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };
  config.program.threads[3] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };

  REQUIRE_THROWS_AS(verify(config), std::invalid_argument);
}

TEST_CASE("verify accepts compact zero-based thread ids", "[algo][dpor]") {
  DporConfig config;
  config.program.threads[0] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "hello"};
    }
    return std::nullopt;
  };
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
}

TEST_CASE("single thread with one send explores 1 execution", "[algo][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "hello"};
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
}

TEST_CASE("single thread with multiple sends explores 1 execution", "[algo][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step < 3) {
      return SendLabel{.destination = 2, .value = "msg"};
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
}

// --- Send-receive pairs ---

TEST_CASE("two threads, one send-receive pair explores 1 execution", "[algo][dpor]") {
  DporConfig config;

  // Thread 1: send to thread 2.
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "x"};
    }
    return std::nullopt;
  };

  // Thread 2: receive (match any).
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
}

// --- Error events ---

TEST_CASE("error event is counted as an error terminal execution", "[algo][dpor]") {
  DporConfig config;

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return ErrorLabel{.message = "boom"};
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.full_executions_explored == 0);
  REQUIRE(result.error_executions_explored == 1);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.executions_explored == 1);
}

// --- ND choices ---

TEST_CASE("ND choice with 2 options explores 2 executions", "[algo][dpor]") {
  DporConfig config;

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b"},
      };
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  require_dpor_matches_oracle(config.program, "T1=[ND({a,b})]");
}

TEST_CASE("ND choice with 3 options explores 3 executions", "[algo][dpor]") {
  DporConfig config;

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b", "c"},
      };
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 3);
}

// --- Two sends to same receiver ---

TEST_CASE("two sends to same receiver explores 2 executions", "[algo][dpor]") {
  DporConfig config;

  // Thread 1: send "a" to thread 3.
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "a"};
    }
    return std::nullopt;
  };

  // Thread 2: send "b" to thread 3.
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "b"};
    }
    return std::nullopt;
  };

  // Thread 3: receive one message (match any).
  config.program.threads[3] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  require_dpor_matches_oracle(config.program, "T1=[S(3,a)]; T2=[S(3,b)]; T3=[Rb(*)]");
}

// --- s+s+r example (paper Example 2.3) ---

TEST_CASE("s+s+r: two sends from one thread, one receive, explores 2 executions", "[algo][dpor]") {
  DporConfig config;

  // Thread 1: send "a" then send "b" to thread 2.
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "a"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "b"};
    }
    return std::nullopt;
  };

  // Thread 2: receive one message (match any).
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  require_dpor_matches_oracle(config.program, "T1=[S(2,a),S(2,b)]; T2=[Rb(*)]");
}

TEST_CASE("receiver-first schedule still explores both rf choices via backward revisit",
          "[algo][dpor][regression]") {
  DporConfig config;

  // T1 has the smallest tid and is considered first by next-event selection.
  // It performs a receive as its first operation.
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  // Two independent senders to T1.
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "a"};
    }
    return std::nullopt;
  };

  config.program.threads[3] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "b"};
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  // One execution where T1 reads from T2's send and one from T3's send.
  REQUIRE(result.executions_explored == 2);
  require_dpor_matches_oracle(config.program, "T1=[Rb(*)]; T2=[S(1,a)]; T3=[S(1,b)]");
}

TEST_CASE("next-event converts an unsatisfied receive into an internal block",
          "[algo][dpor][regression]") {
  Program program;
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto next = dpor::algo::detail::compute_next_event(
      program, ExplorationGraph{}, dpor::algo::detail::sorted_thread_ids(program));
  REQUIRE(next.has_value());
  REQUIRE(next->first == 1);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<BlockLabel>(
      next->second));  // NOLINT(bugprone-unchecked-optional-access)
}

TEST_CASE("next-event does not block an unsatisfied non-blocking receive",
          "[algo][dpor][nonblocking]") {
  Program program;
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_nonblocking_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto next = dpor::algo::detail::compute_next_event(
      program, ExplorationGraph{}, dpor::algo::detail::sorted_thread_ids(program));
  REQUIRE(next.has_value());
  REQUIRE(next->first == 1);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<ReceiveLabel>(
      next->second));                           // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::get<ReceiveLabel>(next->second)  // NOLINT(bugprone-unchecked-optional-access)
              .is_nonblocking());
}

TEST_CASE("non-blocking receive with no sends explores one bottom execution",
          "[algo][dpor][nonblocking]") {
  DporConfig config;
  std::vector<ExplorationGraph> executions;

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_nonblocking_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.on_execution = [&executions](const ExplorationGraph& graph) {
    executions.push_back(graph);
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(executions.size() == 1);

  const auto& graph = executions.front();
  REQUIRE(graph.event_count() == 1);
  REQUIRE(is_receive(graph.event(0)));
  REQUIRE_FALSE(is_block(graph.event(0)));

  const auto rf_it = graph.reads_from().find(0);
  REQUIRE(rf_it != graph.reads_from().end());
  REQUIRE(rf_it->second.is_bottom());
  require_dpor_matches_oracle(config.program, "T1=[Rnb(*)]");
}

TEST_CASE("many non-blocking receives with no sends explore one execution",
          "[algo][dpor][nonblocking]") {
  DporConfig config;
  std::vector<ExplorationGraph> executions;

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (trace.size() != step) {
      throw std::logic_error(
          "non-blocking receive regression: bottom observations must remain in thread_trace");
    }
    if (step < 3) {
      return make_nonblocking_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.on_execution = [&executions](const ExplorationGraph& graph) {
    executions.push_back(graph);
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(executions.size() == 1);

  const auto& graph = executions.front();
  REQUIRE(graph.event_count() == 3);
  REQUIRE_FALSE(std::any_of(graph.events().begin(), graph.events().end(),
                            [](const Event& event) { return is_block(event); }));

  const auto trace = graph.thread_trace(1);
  REQUIRE(trace.size() == 3);
  REQUIRE(std::all_of(trace.begin(), trace.end(),
                      [](const auto& observed) { return observed.is_bottom(); }));
}

TEST_CASE("receiver-first non-blocking receive explores bottom and matched executions",
          "[algo][dpor][nonblocking]") {
  DporConfig config;
  std::vector<ExplorationGraph> executions;

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"x"}, ReceiveMode::NonBlocking);
    }
    return std::nullopt;
  };

  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "x"};
    }
    return std::nullopt;
  };

  config.on_execution = [&executions](const ExplorationGraph& graph) {
    executions.push_back(graph);
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(executions.size() == 2);

  bool saw_bottom = false;
  bool saw_matched = false;

  for (const auto& graph : executions) {
    REQUIRE_FALSE(std::any_of(graph.events().begin(), graph.events().end(),
                              [](const Event& event) { return is_block(event); }));

    const auto recv_id = find_event_id_by_thread_index(graph, 1, 0);
    REQUIRE(recv_id != ExplorationGraph::kNoSource);
    const auto rf_it = graph.reads_from().find(recv_id);
    REQUIRE(rf_it != graph.reads_from().end());

    if (rf_it->second.is_bottom()) {
      saw_bottom = true;
      continue;
    }

    const auto* send = as_send(graph.event(rf_it->second.send_id()));
    REQUIRE(send != nullptr);
    REQUIRE(send->value == "x");
    REQUIRE(send->destination == 1);
    saw_matched = true;
  }

  REQUIRE(saw_bottom);
  REQUIRE(saw_matched);
}

TEST_CASE("backward revisit rewires non-blocking receive from bottom in direct graph test",
          "[algo][dpor][nonblocking]") {
  ExplorationGraph graph;
  const auto recv =
      graph.add_event(1, make_receive_label_from_values<Value>({"x"}, ReceiveMode::NonBlocking));
  graph.set_reads_from_bottom(recv);
  const auto send = graph.add_event(2, SendLabel{.destination = 1, .value = "x"});

  std::vector<ExplorationGraph> revisited_graphs;
  VerifyResult result;
  DporConfig config;
  config.on_execution = [&revisited_graphs](const ExplorationGraph& g) {
    revisited_graphs.push_back(g);
  };

  dpor::algo::detail::backward_revisit(config.program, graph, send, result, config, 0,
                                       dpor::algo::detail::sorted_thread_ids(config.program));

  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(revisited_graphs.size() == 1);

  const auto& revisited = revisited_graphs.front();
  const auto new_recv = find_event_id_by_thread_index(revisited, 1, 0);
  const auto new_send = find_event_id_by_thread_index(revisited, 2, 0);
  REQUIRE(new_recv != ExplorationGraph::kNoSource);
  REQUIRE(new_send != ExplorationGraph::kNoSource);

  const auto rf_it = revisited.reads_from().find(new_recv);
  REQUIRE(rf_it != revisited.reads_from().end());
  REQUIRE(rf_it->second.is_send());
  REQUIRE(rf_it->second.send_id() == new_send);

  const auto* rf_send = as_send(revisited.event(new_send));
  REQUIRE(rf_send != nullptr);
  REQUIRE(rf_send->destination == 1);
  REQUIRE(rf_send->value == "x");
}

TEST_CASE("revisited graphs materialized under backward revisit start with clean rollback history",
          "[algo][dpor][rollback]") {
  ExplorationGraph graph;
  const auto recv =
      graph.add_event(1, make_receive_label_from_values<Value>({"x"}, ReceiveMode::NonBlocking));
  graph.set_reads_from_bottom(recv);
  const auto send = graph.add_event(2, SendLabel{.destination = 1, .value = "x"});

  std::vector<ExplorationGraph::Checkpoint> seen_checkpoints;
  VerifyResult result;
  DporConfig config;
  config.on_execution = [&seen_checkpoints](const ExplorationGraph& g) {
    seen_checkpoints.push_back(g.checkpoint());
  };

  dpor::algo::detail::backward_revisit(config.program, graph, send, result, config, 0,
                                       dpor::algo::detail::sorted_thread_ids(config.program));

  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(seen_checkpoints.size() == 1);
  REQUIRE(seen_checkpoints.front().event_undo_size == 0);
  REQUIRE(seen_checkpoints.front().rf_undo_size == 0);
}

TEST_CASE("send-branch revisit sees the temporary send and leaves the caller graph rolled back",
          "[algo][dpor][rollback][nonblocking]") {
  Program program;
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"x"}, ReceiveMode::NonBlocking);
    }
    return std::nullopt;
  };
  program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "x"};
    }
    return std::nullopt;
  };

  DporConfig config;
  config.program = program;

  bool saw_bottom = false;
  bool saw_matched = false;
  config.on_execution = [&](const ExplorationGraph& g) {
    const auto recv_id = find_event_id_by_thread_index(g, 1, 0);
    REQUIRE(recv_id != ExplorationGraph::kNoSource);

    const auto rf_it = g.reads_from().find(recv_id);
    REQUIRE(rf_it != g.reads_from().end());
    if (rf_it->second.is_bottom()) {
      saw_bottom = true;
      return;
    }

    const auto* matched_send = as_send(g.event(rf_it->second.send_id()));
    REQUIRE(matched_send != nullptr);
    REQUIRE(matched_send->destination == 1);
    REQUIRE(matched_send->value == "x");
    saw_matched = true;
  };

  VerifyResult result;
  ExplorationGraph graph;
  dpor::algo::detail::visit(program, graph, result, config, 0,
                            dpor::algo::detail::sorted_thread_ids(program));

  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(saw_bottom);
  REQUIRE(saw_matched);
  REQUIRE(graph.event_count() == 0);
  REQUIRE(graph.insertion_order().empty());
}

TEST_CASE("non-blocking receive exposes bottom in trace for later control flow",
          "[algo][dpor][nonblocking]") {
  DporConfig config;
  std::set<std::string> observed_pairs;

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_nonblocking_receive_label<Value>();
    }
    if (step == 1 && trace.size() == 1) {
      if (trace[0].is_bottom()) {
        return SendLabel{.destination = 3, .value = "timeout"};
      }
      return SendLabel{.destination = 3, .value = trace[0].value()};
    }
    return std::nullopt;
  };

  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "x"};
    }
    return std::nullopt;
  };

  config.program.threads[3] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label_from_values<Value>({"timeout", "x"});
    }
    return std::nullopt;
  };

  config.on_execution = [&observed_pairs](const ExplorationGraph& graph) {
    const auto t1_trace = graph.thread_trace(1);
    const auto t3_trace = graph.thread_trace(3);
    REQUIRE(t1_trace.size() == 1);
    REQUIRE(t3_trace.size() == 1);

    if (t1_trace[0].is_bottom()) {
      observed_pairs.insert("bottom->" + t3_trace[0].value());
    } else {
      observed_pairs.insert(t1_trace[0].value() + "->" + t3_trace[0].value());
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_pairs == std::set<std::string>{"bottom->timeout", "x->x"});
  require_dpor_matches_oracle(
      config.program,
      "T1=[Rnb(*),S(3,bottom?timeout:trace[0])]; T2=[S(1,x)]; T3=[Rb({timeout,x})]");
}

TEST_CASE("backward-revisit-heavy exploration does not produce duplicate execution graphs",
          "[algo][dpor][regression]") {
  DporConfig config;
  std::vector<std::string> signatures;

  // Receiver thread (smallest tid) performs two receives.
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step < 2 && trace.size() == step) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "a"};
    }
    return std::nullopt;
  };

  config.program.threads[3] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "b"};
    }
    return std::nullopt;
  };

  config.program.threads[4] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "c"};
    }
    return std::nullopt;
  };

  config.on_execution = [&signatures](const ExplorationGraph& graph) {
    signatures.push_back(graph_signature(graph));
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 6);
  REQUIRE(signatures.size() == result.executions_explored);

  const std::set<std::string> unique_signatures(signatures.begin(), signatures.end());
  REQUIRE(unique_signatures.size() == signatures.size());
}

// --- max_depth ---

TEST_CASE("max_depth limits exploration", "[algo][dpor]") {
  DporConfig config;
  config.max_depth = 2;
  std::size_t observed_count = 0;
  bool saw_depth_limit_execution = false;

  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    ++observed_count;
    saw_depth_limit_execution = execution.kind == TerminalExecutionKind::DepthLimit;
  };

  // Thread that sends indefinitely.
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return SendLabel{.destination = 2, .value = "x"};
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.full_executions_explored == 0);
  REQUIRE(result.error_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 1);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(observed_count == 1);
  REQUIRE(saw_depth_limit_execution);
  // With max_depth=2, it should stop early rather than looping forever.
}

// --- Terminal execution observer ---

TEST_CASE("terminal execution observer is called for each full execution", "[algo][dpor]") {
  DporConfig config;
  std::size_t observed_count = 0;

  config.on_terminal_execution = [&observed_count](const TerminalExecution& execution) {
    REQUIRE(execution.kind == TerminalExecutionKind::Full);
    ++observed_count;
  };

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b"},
      };
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.full_executions_explored == 2);
  REQUIRE(result.error_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_count == 2);
}

TEST_CASE("terminal execution observer can stop sequential exploration", "[algo][dpor]") {
  DporConfig config;
  std::size_t observed_count = 0;

  config.on_terminal_execution = [&observed_count](const TerminalExecution& execution) {
    REQUIRE(execution.kind == TerminalExecutionKind::Full);
    ++observed_count;
    return TerminalExecutionAction::Stop;
  };

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b"},
      };
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::Stopped);
  REQUIRE(result.full_executions_explored == 1);
  REQUIRE(result.error_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(observed_count == 1);
}

TEST_CASE("progress observer reports running and final sequential snapshots", "[algo][dpor]") {
  DporConfig config;
  std::vector<ProgressSnapshot> snapshots;

  config.progress_report_interval = std::chrono::milliseconds::zero();
  config.on_progress = [&snapshots](const ProgressSnapshot& snapshot) {
    snapshots.push_back(snapshot);
  };
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b", "c"},
      };
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE_FALSE(snapshots.empty());

  bool saw_running = false;
  for (const auto& snapshot : snapshots) {
    if (snapshot.state != ProgressState::Running) {
      continue;
    }
    saw_running = true;
    REQUIRE(snapshot.counts_exact);
    REQUIRE(snapshot.active_workers == 1);
    REQUIRE(snapshot.max_workers == 1);
    REQUIRE(snapshot.queued_tasks == 0);
    REQUIRE(snapshot.max_queued_tasks == 0);
  }

  REQUIRE(saw_running);
  const auto& final_snapshot = snapshots.back();
  REQUIRE(final_snapshot.state == ProgressState::AllExplored);
  REQUIRE(final_snapshot.counts_exact);
  REQUIRE(final_snapshot.terminal_executions == result.executions_explored);
  REQUIRE(final_snapshot.full_executions == result.full_executions_explored);
  REQUIRE(final_snapshot.error_executions == result.error_executions_explored);
  REQUIRE(final_snapshot.depth_limit_executions == result.depth_limit_executions_explored);
  REQUIRE(final_snapshot.active_workers == 0);
  REQUIRE(final_snapshot.max_workers == 1);
  REQUIRE(final_snapshot.queued_tasks == 0);
  REQUIRE(final_snapshot.max_queued_tasks == 0);
}

TEST_CASE("detail visit shows a full execution to the observer before rollback",
          "[algo][dpor][rollback]") {
  Program program;
  program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "x"};
    }
    return std::nullopt;
  };

  DporConfig config;
  config.program = program;

  std::size_t observed_count = 0;
  config.on_terminal_execution = [&observed_count](const TerminalExecution& execution) {
    REQUIRE(execution.kind == TerminalExecutionKind::Full);
    const auto& graph = execution.graph;
    ++observed_count;
    REQUIRE(graph.event_count() == 1);
    REQUIRE(is_send(graph.event(0)));
  };

  VerifyResult result;
  ExplorationGraph graph;
  dpor::algo::detail::visit(program, graph, result, config, 0,
                            dpor::algo::detail::sorted_thread_ids(program));

  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.full_executions_explored == 1);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(observed_count == 1);
  REQUIRE(graph.event_count() == 0);
  REQUIRE(graph.insertion_order().empty());
}

TEST_CASE("detail visit shows the error execution to the observer before rollback",
          "[algo][dpor][rollback]") {
  Program program;
  program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return ErrorLabel{.message = "observer-visible error"};
    }
    return std::nullopt;
  };

  DporConfig config;
  config.program = program;

  std::size_t observed_count = 0;
  config.on_terminal_execution = [&observed_count](const TerminalExecution& execution) {
    REQUIRE(execution.kind == TerminalExecutionKind::Error);
    const auto& graph = execution.graph;
    ++observed_count;
    REQUIRE(graph.event_count() == 1);
    REQUIRE(is_error(graph.event(0)));
    REQUIRE(as_error(graph.event(0))->message == "observer-visible error");
  };

  VerifyResult result;
  ExplorationGraph graph;
  dpor::algo::detail::visit(program, graph, result, config, 0,
                            dpor::algo::detail::sorted_thread_ids(program));

  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.error_executions_explored == 1);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(observed_count == 1);
  REQUIRE(graph.event_count() == 0);
  REQUIRE(graph.insertion_order().empty());
}

// --- Cycle-inducing rf pruned ---

TEST_CASE("cycle-inducing rf assignment is pruned by consistency check", "[algo][dpor]") {
  DporConfig config;

  // Thread 1: send to thread 2, then receive from thread 2.
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "a"};
    }
    if (step == 1) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  // Thread 2: send to thread 1, then receive from thread 1.
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "b"};
    }
    if (step == 1) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  // Both threads send first, then receive — acyclic.
  REQUIRE(result.executions_explored >= 1);
}

// --- Three-thread chain ---

TEST_CASE("three-thread chain: thread 1 sends to 2, thread 2 forwards to 3", "[algo][dpor]") {
  DporConfig config;

  // Thread 1: send to thread 2.
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "x"};
    }
    return std::nullopt;
  };

  // Thread 2: receive then send to thread 3.
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    if (step == 1 && trace.size() == 1) {
      return SendLabel{.destination = 3, .value = trace[0].value()};
    }
    return std::nullopt;
  };

  // Thread 3: receive.
  config.program.threads[3] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
}

// --- Block events ---

TEST_CASE("program thread function returning BlockLabel is rejected", "[algo][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return BlockLabel{};
    }
    return std::nullopt;
  };

  REQUIRE_THROWS_AS(verify(config), std::logic_error);
}

TEST_CASE("internal block suspends thread progress until receive is rescheduled",
          "[algo][dpor][regression]") {
  Program program;

  // T1: blocking receive, then send to T3.
  program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    if (step == 1) {
      return SendLabel{.destination = 3, .value = "done"};
    }
    return std::nullopt;
  };

  // T2: provide the message that eventually unblocks T1.
  program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "msg"};
    }
    return std::nullopt;
  };

  // T3: receive T1's "done".
  program.threads[3] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label_from_values<Value>({"done"});
    }
    return std::nullopt;
  };

  // At the empty graph, T1 has no compatible send and must be internally blocked.
  const auto first = dpor::algo::detail::compute_next_event(
      program, ExplorationGraph{}, dpor::algo::detail::sorted_thread_ids(program));
  REQUIRE(first.has_value());
  REQUIRE(first->first == 1);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<BlockLabel>(
      first->second));  // NOLINT(bugprone-unchecked-optional-access)

  DporConfig config;
  config.program = program;

  bool saw_completed_graph_with_block = false;
  bool saw_bad_t1_prefix = false;
  bool saw_bad_t3_receive = false;

  config.on_execution = [&](const ExplorationGraph& graph) {
    for (const auto& evt : graph.events()) {
      if (is_block(evt)) {
        saw_completed_graph_with_block = true;
      }
    }

    const auto t1_e0 = find_event_id_by_thread_index(graph, 1, 0);
    const auto t1_e1 = find_event_id_by_thread_index(graph, 1, 1);
    if (t1_e0 == ExplorationGraph::kNoSource || t1_e1 == ExplorationGraph::kNoSource ||
        !is_receive(graph.event(t1_e0))) {
      saw_bad_t1_prefix = true;
      return;
    }

    const auto* t1_send = as_send(graph.event(t1_e1));
    if (t1_send == nullptr || t1_send->destination != 3 || t1_send->value != "done") {
      saw_bad_t1_prefix = true;
      return;
    }

    const auto t3_recv = find_event_id_by_thread_index(graph, 3, 0);
    if (t3_recv == ExplorationGraph::kNoSource || !is_receive(graph.event(t3_recv))) {
      saw_bad_t3_receive = true;
      return;
    }
    const auto rf_it = graph.reads_from().find(t3_recv);
    if (rf_it == graph.reads_from().end()) {
      saw_bad_t3_receive = true;
      return;
    }

    const auto* src = as_send(graph.event(rf_it->second.send_id()));
    if (src == nullptr || src->value != "done" ||
        graph.event(rf_it->second.send_id()).thread != 1) {
      saw_bad_t3_receive = true;
      return;
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE_FALSE(saw_completed_graph_with_block);
  REQUIRE_FALSE(saw_bad_t1_prefix);
  REQUIRE_FALSE(saw_bad_t3_receive);
  require_dpor_matches_oracle(config.program,
                              "T1=[Rb(*),S(3,done)]; T2=[S(1,msg)]; T3=[Rb({done})]");
}

TEST_CASE("multiple blocked receives are both rescheduled when matching sends appear",
          "[algo][dpor][regression]") {
  Program program;

  // Two receiver threads that start blocked until sends arrive.
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"a", "a2"});
    }
    return std::nullopt;
  };
  program.threads[2] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"b", "b2"});
    }
    return std::nullopt;
  };

  // Two senders that eventually provide matching values for both receivers.
  program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "a"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "b"};
    }
    return std::nullopt;
  };
  program.threads[4] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "a2"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "b2"};
    }
    return std::nullopt;
  };

  // At the beginning, both receiver threads should become internally blocked.
  ExplorationGraph partial;
  const auto first = dpor::algo::detail::compute_next_event(
      program, partial, dpor::algo::detail::sorted_thread_ids(program));
  REQUIRE(first.has_value());
  REQUIRE(first->first == 1);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<BlockLabel>(
      first->second));  // NOLINT(bugprone-unchecked-optional-access)
  static_cast<void>(partial.add_event(
      first->first, first->second));  // NOLINT(bugprone-unchecked-optional-access)

  const auto second = dpor::algo::detail::compute_next_event(
      program, partial, dpor::algo::detail::sorted_thread_ids(program));
  REQUIRE(second.has_value());
  REQUIRE(second->first == 2);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<BlockLabel>(
      second->second));  // NOLINT(bugprone-unchecked-optional-access)

  DporConfig config;
  config.program = program;

  bool saw_completed_graph_with_block = false;
  bool missing_receiver_event = false;
  std::set<std::string> t1_received_values;
  std::set<std::string> t2_received_values;

  config.on_execution = [&](const ExplorationGraph& graph) {
    for (const auto& evt : graph.events()) {
      if (is_block(evt)) {
        saw_completed_graph_with_block = true;
      }
    }

    const auto t1_recv = find_event_id_by_thread_index(graph, 1, 0);
    const auto t2_recv = find_event_id_by_thread_index(graph, 2, 0);
    if (t1_recv == ExplorationGraph::kNoSource || t2_recv == ExplorationGraph::kNoSource ||
        !is_receive(graph.event(t1_recv)) || !is_receive(graph.event(t2_recv))) {
      missing_receiver_event = true;
      return;
    }

    const auto rf1 = graph.reads_from().find(t1_recv);
    const auto rf2 = graph.reads_from().find(t2_recv);
    if (rf1 == graph.reads_from().end() || rf2 == graph.reads_from().end()) {
      missing_receiver_event = true;
      return;
    }

    const auto* src1 = as_send(graph.event(rf1->second.send_id()));
    const auto* src2 = as_send(graph.event(rf2->second.send_id()));
    if (src1 == nullptr || src2 == nullptr) {
      missing_receiver_event = true;
      return;
    }

    t1_received_values.insert(src1->value);
    t2_received_values.insert(src2->value);
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 4);
  REQUIRE_FALSE(saw_completed_graph_with_block);
  REQUIRE_FALSE(missing_receiver_event);
  REQUIRE(t1_received_values == std::set<std::string>{"a", "a2"});
  REQUIRE(t2_received_values == std::set<std::string>{"b", "b2"});
  require_dpor_matches_oracle(
      config.program, "T1=[Rb({a,a2})]; T2=[Rb({b,b2})]; T3=[S(1,a),S(2,b)]; T4=[S(1,a2),S(2,b2)]");
}

// --- ND choice affects subsequent behavior ---

TEST_CASE("ND choice value visible in subsequent trace", "[algo][dpor]") {
  DporConfig config;
  std::vector<std::string> observed_values;

  config.on_execution = [&observed_values](const ExplorationGraph& g) {
    // Find sends and record their values.
    for (const auto& evt : g.events()) {
      if (const auto* send = as_send(evt)) {
        observed_values.push_back(send->value);
      }
    }
  };

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b"},
      };
    }
    if (step == 1 && trace.size() == 1) {
      // Send the ND choice value.
      return SendLabel{.destination = 2, .value = trace[0].value()};
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_values.size() == 2);
  // One execution sends "a", the other sends "b".
  std::sort(observed_values.begin(), observed_values.end());
  REQUIRE(observed_values[0] == "a");
  REQUIRE(observed_values[1] == "b");
}

// --- Algorithmic regressions against Must-style constraints ---

TEST_CASE("all explored executions should satisfy async consistency", "[algo][dpor][regression]") {
  DporConfig config;

  // T1: R
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  // T2: S(3,c); S(1,a)
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "c"};
    }
    if (step == 1) {
      return SendLabel{.destination = 1, .value = "a"};
    }
    return std::nullopt;
  };

  // T3: R; S(1,b)
  config.program.threads[3] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    if (step == 1) {
      return SendLabel{.destination = 1, .value = "b"};
    }
    return std::nullopt;
  };

  // T4: ND{b,a}; S(3,b)
  config.program.threads[4] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"b", "a"},
      };
    }
    if (step == 1) {
      return SendLabel{.destination = 3, .value = "b"};
    }
    return std::nullopt;
  };

  AsyncConsistencyChecker checker;
  bool found_inconsistent = false;

  config.on_execution = [&](const ExplorationGraph& g) {
    const auto consistency = checker.check(g.execution_graph());
    if (!consistency.is_consistent()) {
      found_inconsistent = true;
    }
  };

  static_cast<void>(verify(config));
  REQUIRE_FALSE(found_inconsistent);
}

TEST_CASE("backward revisit must reject candidates when a deleted event violates revisit condition",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  // 0: T2 send -> T3
  const auto s20 = graph.add_event(2, SendLabel{.destination = 3, .value = "c"});
  // 1: T2 send -> T1
  static_cast<void>(graph.add_event(2, SendLabel{.destination = 1, .value = "a"}));
  // 2: T1 receive
  const auto r10 = graph.add_event(1, make_receive_label<Value>());
  // 3: T3 receive
  const auto r30 = graph.add_event(3, make_receive_label<Value>());
  // 4: T4 ND
  static_cast<void>(graph.add_event(4, NondeterministicChoiceLabel{
                                           .value = "b",
                                           .choices = {"b", "a"},
                                       }));
  // 5: T4 send -> T3 (candidate revisiting send)
  const auto s41 = graph.add_event(4, SendLabel{.destination = 3, .value = "b"});
  // 6: T3 send -> T1
  const auto s31 = graph.add_event(3, SendLabel{.destination = 1, .value = "b"});

  // r10 currently reads from s31, r30 currently reads from s20.
  graph.set_reads_from(r10, s31);
  graph.set_reads_from(r30, s20);

  // Revisit candidate is (r30, s41). The deleted event set contains s31,
  // and RevisitCondition(G, s31, s41) should fail because r10 reads from s31.
  REQUIRE(dpor::algo::detail::revisit_condition(graph, r30, s41));
  REQUIRE_FALSE(dpor::algo::detail::revisit_condition(graph, s31, s41));

  VerifyResult result;
  DporConfig config;
  dpor::algo::detail::backward_revisit(config.program, graph, s41, result, config, 0,
                                       dpor::algo::detail::sorted_thread_ids(config.program));

  // With the Deleted-set guard from Algorithm 1, this revisit should be blocked.
  REQUIRE(result.executions_explored == 0);
}

TEST_CASE("line-10 revisit filter blocks revisits when receive already reaches the new send",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  const auto source = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto receive = graph.add_event(2, make_receive_label<Value>());
  const auto send = graph.add_event(2, SendLabel{.destination = 2, .value = "x"});
  graph.set_reads_from(receive, source);

  REQUIRE(graph.porf_contains(receive, send));

  VerifyResult result;
  DporConfig config;
  dpor::algo::detail::backward_revisit(config.program, graph, send, result, config, 0,
                                       dpor::algo::detail::sorted_thread_ids(config.program));
  REQUIRE(result.executions_explored == 0);
}

TEST_CASE("deleted-set check rejects revisit when any deleted event fails (mixed event kinds)",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  const auto s20 = graph.add_event(2, SendLabel{.destination = 3, .value = "c"});
  const auto s80 = graph.add_event(8, SendLabel{.destination = 7, .value = "u"});
  const auto r10 = graph.add_event(1, make_receive_label<Value>());
  const auto r30 = graph.add_event(3, make_receive_label<Value>());
  const auto r70 = graph.add_event(7, make_receive_label<Value>());
  const auto nd_bad = graph.add_event(6, NondeterministicChoiceLabel{
                                             .value = "b",
                                             .choices = {"a", "b"},
                                         });
  static_cast<void>(graph.add_event(5, SendLabel{.destination = 1, .value = "z"}));
  const auto s41 = graph.add_event(4, SendLabel{.destination = 3, .value = "b"});
  const auto s31 = graph.add_event(3, SendLabel{.destination = 1, .value = "b"});

  graph.set_reads_from(r10, s31);
  graph.set_reads_from(r30, s20);
  graph.set_reads_from(r70, s80);

  REQUIRE(dpor::algo::detail::revisit_condition(graph, r30, s41));
  REQUIRE_FALSE(dpor::algo::detail::revisit_condition(graph, nd_bad, s41));
  REQUIRE(dpor::algo::detail::revisit_condition(graph, r70, s41));

  VerifyResult result;
  DporConfig config;
  dpor::algo::detail::backward_revisit(config.program, graph, s41, result, config, 0,
                                       dpor::algo::detail::sorted_thread_ids(config.program));
  REQUIRE(result.executions_explored == 0);
}

TEST_CASE("backward revisit preserves intended rf endpoint after restrict/remap",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  const auto s_old = graph.add_event(1, SendLabel{.destination = 4, .value = "old"});
  static_cast<void>(s_old);
  const auto receive = graph.add_event(4, make_receive_label<Value>());
  static_cast<void>(graph.add_event(3, SendLabel{.destination = 9, .value = "noise"}));
  const auto s_new = graph.add_event(2, SendLabel{.destination = 4, .value = "new"});
  graph.set_reads_from(receive, s_old);

  std::vector<ExplorationGraph> revisited_graphs;
  VerifyResult result;
  DporConfig config;
  config.on_execution = [&revisited_graphs](const ExplorationGraph& g) {
    revisited_graphs.push_back(g);
  };

  dpor::algo::detail::backward_revisit(config.program, graph, s_new, result, config, 0,
                                       dpor::algo::detail::sorted_thread_ids(config.program));

  REQUIRE(result.executions_explored == 1);
  REQUIRE(revisited_graphs.size() == 1);

  const auto& revisited = revisited_graphs.front();
  REQUIRE(revisited.event_count() == 3);
  REQUIRE(find_event_id_by_thread_index(revisited, 3, 0) == ExplorationGraph::kNoSource);

  const auto new_receive_id = find_event_id_by_thread_index(revisited, 4, 0);
  REQUIRE(new_receive_id != ExplorationGraph::kNoSource);

  const auto rf_it = revisited.reads_from().find(new_receive_id);
  REQUIRE(rf_it != revisited.reads_from().end());

  const auto* rf_send = as_send(revisited.event(rf_it->second.send_id()));
  REQUIRE(rf_send != nullptr);
  REQUIRE(rf_send->destination == 4);
  REQUIRE(rf_send->value == "new");
}

TEST_CASE("only compatible receives in destination thread are revisited",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  const auto s_b = graph.add_event(2, SendLabel{.destination = 4, .value = "b"});
  const auto r_b = graph.add_event(4, make_receive_label_from_values<Value>({"b"}));
  const auto s_a = graph.add_event(1, SendLabel{.destination = 4, .value = "a"});
  const auto r_a = graph.add_event(4, make_receive_label_from_values<Value>({"a"}));
  const auto s_new = graph.add_event(3, SendLabel{.destination = 4, .value = "a"});

  graph.set_reads_from(r_b, s_b);
  graph.set_reads_from(r_a, s_a);

  std::vector<ExplorationGraph> revisited_graphs;
  VerifyResult result;
  DporConfig config;
  config.on_execution = [&revisited_graphs](const ExplorationGraph& g) {
    revisited_graphs.push_back(g);
  };

  dpor::algo::detail::backward_revisit(config.program, graph, s_new, result, config, 0,
                                       dpor::algo::detail::sorted_thread_ids(config.program));

  REQUIRE(result.executions_explored == 1);
  REQUIRE(revisited_graphs.size() == 1);

  const auto& revisited = revisited_graphs.front();
  const auto rb_id = find_event_id_by_thread_index(revisited, 4, 0);
  const auto ra_id = find_event_id_by_thread_index(revisited, 4, 1);
  REQUIRE(rb_id != ExplorationGraph::kNoSource);
  REQUIRE(ra_id != ExplorationGraph::kNoSource);

  const auto rb_rf = revisited.reads_from().find(rb_id);
  const auto ra_rf = revisited.reads_from().find(ra_id);
  REQUIRE(rb_rf != revisited.reads_from().end());
  REQUIRE(ra_rf != revisited.reads_from().end());

  const auto* rb_send = as_send(revisited.event(rb_rf->second.send_id()));
  const auto* ra_send = as_send(revisited.event(ra_rf->second.send_id()));
  REQUIRE(rb_send != nullptr);
  REQUIRE(ra_send != nullptr);

  REQUIRE(rb_send->value == "b");
  REQUIRE(rb_send->destination == 4);
  REQUIRE(ra_send->value == "a");
  REQUIRE(ra_send->destination == 4);
  REQUIRE(revisited.event(ra_rf->second.send_id()).thread == 3);
}

TEST_CASE("tiebreaker should not pick an already-consumed send", "[algo][dpor][regression]") {
  ExplorationGraph graph;

  const auto s1 = graph.add_event(1, SendLabel{.destination = 3, .value = "x"});
  const auto r0 = graph.add_event(3, make_receive_label<Value>());
  const auto s2 = graph.add_event(2, SendLabel{.destination = 3, .value = "y"});
  const auto r1 = graph.add_event(3, make_receive_label<Value>());

  graph.set_reads_from(r0, s1);  // s1 already consumed
  graph.set_reads_from(r1, s2);  // r1 must have a current source under blocking semantics

  const auto chosen = dpor::algo::detail::get_cons_tiebreaker(graph, r1);
  REQUIRE(chosen == s2);
}

TEST_CASE("fifo p2p tiebreaker skips FIFO-inconsistent smaller-tid send",
          "[algo][dpor][fifo_p2p][regression]") {
  ExplorationGraph graph;

  const auto s10 = graph.add_event(1, SendLabel{.destination = 3, .value = "a"});
  const auto s11 = graph.add_event(1, SendLabel{.destination = 3, .value = "b"});
  const auto s20 = graph.add_event(2, SendLabel{.destination = 3, .value = "c"});
  const auto r0 = graph.add_event(3, make_receive_label<Value>());
  const auto r1 = graph.add_event(3, make_receive_label_from_values<Value>({"a"}));

  graph.set_reads_from(r0, s20);
  graph.set_reads_from(r1, s10);

  REQUIRE(dpor::algo::detail::get_cons_tiebreaker(graph, r0, CommunicationModel::Async) == s11);
  REQUIRE(dpor::algo::detail::get_cons_tiebreaker(graph, r0, CommunicationModel::FifoP2P) ==
          s20);
}

TEST_CASE("fifo rf rewrite helper accepts safe rewires on known-acyclic graphs",
          "[algo][dpor][fifo_p2p][regression]") {
  ExplorationGraph graph;

  const auto s10 = graph.add_event(1, SendLabel{.destination = 3, .value = "a"});
  const auto s20 = graph.add_event(2, SendLabel{.destination = 3, .value = "a"});
  const auto r0 = graph.add_event(3, make_receive_label_from_values<Value>({"a"}));

  graph.set_reads_from(r0, s10);
  REQUIRE(graph.is_known_acyclic());

  REQUIRE(dpor::algo::detail::rf_rewrite_is_consistent(graph, r0, s20,
                                                       CommunicationModel::FifoP2P));
}

TEST_CASE("tiebreaker should skip compatible sends that would create a cycle",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  const auto s_current = graph.add_event(4, SendLabel{.destination = 1, .value = "x"});
  const auto receive = graph.add_event(1, make_receive_label_from_values<Value>({"x"}));
  const auto s_mid = graph.add_event(1, SendLabel{.destination = 2, .value = "chain"});
  const auto r_mid = graph.add_event(2, make_receive_label_from_values<Value>({"chain"}));
  const auto s_bad = graph.add_event(2, SendLabel{.destination = 1, .value = "x"});
  const auto s_good = graph.add_event(3, SendLabel{.destination = 1, .value = "x"});

  graph.set_reads_from(receive, s_current);
  graph.set_reads_from(r_mid, s_mid);

  REQUIRE_FALSE(graph.has_causal_cycle());
  REQUIRE(graph.porf_contains(receive, s_bad));
  REQUIRE_FALSE(graph.porf_contains(receive, s_good));

  const auto chosen = dpor::algo::detail::get_cons_tiebreaker(graph, receive);
  REQUIRE(chosen == s_good);
}

TEST_CASE("receive revisit condition should use tiebreaker from G|Previous",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  // Current source for the receive.
  const auto send_current = graph.add_event(2, SendLabel{.destination = 1, .value = "x"});
  const auto receive = graph.add_event(1, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(receive, send_current);

  // Compatible send added after the receive, but unrelated to the revisiting send.
  const auto send_outside_previous = graph.add_event(0, SendLabel{.destination = 1, .value = "x"});

  // Candidate revisiting send to an unrelated destination so it does not pull
  // send_outside_previous into Previous.
  const auto revisiting_send = graph.add_event(3, SendLabel{.destination = 9, .value = "y"});

  const auto previous = dpor::algo::detail::compute_previous_set(graph, receive, revisiting_send);
  REQUIRE(previous[send_outside_previous] == 0U);

  const auto restricted = dpor::model::detail::restrict_masked(graph, previous);
  const auto recv_in_previous = find_event_id_by_thread_index(restricted, 1, 0);
  REQUIRE(recv_in_previous != ExplorationGraph::kNoSource);

  // Paper condition: receive should compare against GetConsTiebreaker(G|Previous, e).
  const auto expected_tiebreaker =
      dpor::algo::detail::get_cons_tiebreaker(restricted, recv_in_previous);
  const auto rf_it = restricted.reads_from().find(recv_in_previous);
  REQUIRE(rf_it != restricted.reads_from().end());
  REQUIRE(rf_it->second == expected_tiebreaker);
  REQUIRE(dpor::algo::detail::get_cons_tiebreaker_masked(graph, previous, receive) ==
          send_current);

  // This should hold when the tiebreaker is computed on G|Previous.
  REQUIRE(dpor::algo::detail::revisit_condition(graph, receive, revisiting_send));
}

TEST_CASE("masked tiebreaker should skip deleted intermediate same-thread events",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  const auto send_current = graph.add_event(4, SendLabel{.destination = 1, .value = "x"});
  const auto receive = graph.add_event(1, make_receive_label_from_values<Value>({"x"}));
  const auto deleted_mid = graph.add_event(1, SendLabel{.destination = 9, .value = "noise"});
  const auto send_path = graph.add_event(1, SendLabel{.destination = 2, .value = "chain"});
  const auto recv_path = graph.add_event(2, make_receive_label_from_values<Value>({"chain"}));
  const auto send_bad = graph.add_event(2, SendLabel{.destination = 1, .value = "x"});
  const auto send_good = graph.add_event(3, SendLabel{.destination = 1, .value = "x"});

  graph.set_reads_from(receive, send_current);
  graph.set_reads_from(recv_path, send_path);

  std::vector<std::uint8_t> keep_mask(graph.event_count(), 1);
  keep_mask[deleted_mid] = 0U;

  const auto restricted = dpor::model::detail::restrict_masked(graph, keep_mask);
  const auto recv_in_restricted = find_event_id_by_thread_index(restricted, 1, 0);
  REQUIRE(recv_in_restricted != ExplorationGraph::kNoSource);

  const auto expected_tiebreaker =
      dpor::algo::detail::get_cons_tiebreaker(restricted, recv_in_restricted);
  const auto masked_tiebreaker =
      dpor::algo::detail::get_cons_tiebreaker_masked(graph, keep_mask, receive);
  const auto* restricted_send = as_send(restricted.event(expected_tiebreaker));
  const auto* masked_send = as_send(graph.event(masked_tiebreaker));

  REQUIRE(expected_tiebreaker != ExplorationGraph::kNoSource);
  REQUIRE(masked_tiebreaker != send_bad);
  REQUIRE(masked_tiebreaker == send_good);
  REQUIRE(restricted_send != nullptr);
  REQUIRE(masked_send != nullptr);
  REQUIRE(restricted.event(expected_tiebreaker).thread == graph.event(masked_tiebreaker).thread);
  REQUIRE(restricted_send->value == masked_send->value);
  REQUIRE(restricted_send->destination == masked_send->destination);
  REQUIRE(restricted.event(expected_tiebreaker).thread == 3);
}

TEST_CASE("receive revisit condition rejects rf source outside G|Previous",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;

  // Receive is inserted before its source send.
  const auto receive = graph.add_event(1, make_receive_label_from_values<Value>({"x"}));
  const auto source_outside_previous =
      graph.add_event(2, SendLabel{.destination = 1, .value = "x"});
  const auto revisiting_send = graph.add_event(3, SendLabel{.destination = 9, .value = "y"});

  graph.set_reads_from(receive, source_outside_previous);

  // For this pair (receive, revisiting_send), Previous does not include the
  // current rf source.
  const auto previous = dpor::algo::detail::compute_previous_set(graph, receive, revisiting_send);
  REQUIRE(previous[source_outside_previous] == 0U);

  // Under Algorithm 1, rf(e) cannot equal a tiebreaker computed on G|Previous
  // if rf(e) is outside Previous.
  REQUIRE_FALSE(dpor::algo::detail::revisit_condition(graph, receive, revisiting_send));
}

TEST_CASE("non-blocking receive revisit condition requires bottom source",
          "[algo][dpor][nonblocking]") {
  ExplorationGraph graph;
  const auto receive = graph.add_event(1, make_nonblocking_receive_label<Value>());
  const auto send = graph.add_event(2, SendLabel{.destination = 1, .value = "x"});

  graph.set_reads_from_bottom(receive);
  REQUIRE(dpor::algo::detail::revisit_condition(graph, receive, send));

  graph.set_reads_from(receive, send);
  REQUIRE_FALSE(dpor::algo::detail::revisit_condition(graph, receive, send));
}

TEST_CASE("ND revisit condition should use min(S), not insertion order",
          "[algo][dpor][regression]") {
  ExplorationGraph graph;
  const auto nd = graph.add_event(1, NondeterministicChoiceLabel{
                                         .value = "a",
                                         .choices = {"b", "a"},
                                     });
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});

  // Under the paper's condition val(e) = min(S), this should hold.
  REQUIRE(dpor::algo::detail::revisit_condition(graph, nd, s));
}

// --- Paper examples (Must, OOPSLA'24) within current async + ND scope ---

TEST_CASE("paper ex 2.4: nondet failure target yields one execution per choice",
          "[algo][dpor][paper]") {
  DporConfig config;
  std::set<std::string> received_failures;

  // T1 (environment): who := nondet({node1,node2}); send(T2, Fail(who))
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "node1",
          .choices = {"node1", "node2"},
      };
    }
    if (step == 1 && trace.size() == 1) {
      return SendLabel{
          .destination = 2,
          .value = "Fail(" + trace[0].value() + ")",
      };
    }
    return std::nullopt;
  };

  // T2 (coordinator): receive one failure notification.
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.on_execution = [&received_failures](const ExplorationGraph& graph) {
    const auto trace = graph.thread_trace(2);
    if (!trace.empty()) {
      received_failures.insert(trace[0].value());
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(received_failures == std::set<std::string>{"Fail(node1)", "Fail(node2)"});
  require_dpor_matches_oracle(config.program,
                              "T1=[ND({node1,node2}),S(2,Fail(trace[0]))]; T2=[Rb(*)]");
}

TEST_CASE("paper ex 2.6: selective receive filters stale value", "[algo][dpor][paper]") {
  DporConfig config;

  // Two messages to T3: one stale, one fresh.
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "stale"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "fresh"};
    }
    return std::nullopt;
  };

  // T3 only accepts "fresh".
  config.program.threads[3] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label_from_values<Value>({"fresh"});
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  require_dpor_matches_oracle(config.program, "T1=[S(3,stale)]; T2=[S(3,fresh)]; T3=[Rb({fresh})]");
}

TEST_CASE("paper ex 2.7: ordered selective receives collapse ns+rn-sel to one execution",
          "[algo][dpor][paper]") {
  DporConfig config;

  // T1..T3 send 1..3 to T4.
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "1"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "2"};
    }
    return std::nullopt;
  };
  config.program.threads[3] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "3"};
    }
    return std::nullopt;
  };

  // T4 receives exactly 1, then 2, then 3.
  config.program.threads[4] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"1"});
    }
    if (step == 1 && trace.size() == 1) {
      return make_receive_label_from_values<Value>({"2"});
    }
    if (step == 2 && trace.size() == 2) {
      return make_receive_label_from_values<Value>({"3"});
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  require_dpor_matches_oracle(
      config.program, "T1=[S(4,1)]; T2=[S(4,2)]; T3=[S(4,3)]; T4=[Rb({1}),Rb({2}),Rb({3})]");
}

TEST_CASE("paper ex 2.8: receives can consume messages out of sender order with predicates",
          "[algo][dpor][paper]") {
  DporConfig config;

  // T1: send(T2,1); send(T2,2)
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "1"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "2"};
    }
    return std::nullopt;
  };

  // T2: recv(x==2); recv(x==1)
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"2"});
    }
    if (step == 1 && trace.size() == 1) {
      return make_receive_label_from_values<Value>({"1"});
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  require_dpor_matches_oracle(config.program, "T1=[S(2,1),S(2,2)]; T2=[Rb({2}),Rb({1})]");
}

TEST_CASE("fifo p2p enforces FIFO for same-sender receive choices", "[algo][dpor][fifo_p2p]") {
  DporConfig config;
  config.communication_model = CommunicationModel::FifoP2P;
  std::set<std::string> observed_receive_values;

  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "a"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "b"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.on_execution = [&observed_receive_values](const ExplorationGraph& graph) {
    const auto trace = graph.thread_trace(2);
    if (!trace.empty() && !trace[0].is_bottom()) {
      observed_receive_values.insert(trace[0].value());
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(observed_receive_values == std::set<std::string>{"a"});
  require_dpor_matches_oracle(
      config.program, "fifo_p2p: T1=[S(2,a),S(2,b)]; T2=[Rb(*)]", CommunicationModel::FifoP2P);
}

TEST_CASE("fifo p2p still allows selective receives to consume later matching sends",
          "[algo][dpor][fifo_p2p]") {
  DporConfig config;
  config.communication_model = CommunicationModel::FifoP2P;
  std::vector<std::string> observed_trace;

  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "1"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "2"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"2"});
    }
    if (step == 1 && trace.size() == 1) {
      return make_receive_label_from_values<Value>({"1"});
    }
    return std::nullopt;
  };

  config.on_execution = [&observed_trace](const ExplorationGraph& graph) {
    observed_trace.clear();
    for (const auto& value : graph.thread_trace(2)) {
      observed_trace.push_back(value.value());
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(observed_trace == std::vector<std::string>{"2", "1"});
  require_dpor_matches_oracle(
      config.program, "fifo_p2p: T1=[S(2,1),S(2,2)]; T2=[Rb({2}),Rb({1})]",
      CommunicationModel::FifoP2P);
}

TEST_CASE("fifo p2p permits different senders to the same destination in cross-sender order",
          "[algo][dpor][fifo_p2p]") {
  DporConfig config;
  config.communication_model = CommunicationModel::FifoP2P;
  std::vector<std::string> observed_trace;

  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "a"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "b"};
    }
    return std::nullopt;
  };
  config.program.threads[3] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"b"});
    }
    if (step == 1 && trace.size() == 1) {
      return make_receive_label_from_values<Value>({"a"});
    }
    return std::nullopt;
  };

  config.on_execution = [&observed_trace](const ExplorationGraph& graph) {
    observed_trace.clear();
    for (const auto& value : graph.thread_trace(3)) {
      observed_trace.push_back(value.value());
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(observed_trace == std::vector<std::string>{"b", "a"});
  require_dpor_matches_oracle(
      config.program, "fifo_p2p: T1=[S(3,a)]; T2=[S(3,b)]; T3=[Rb({b}),Rb({a})]",
      CommunicationModel::FifoP2P);
}

TEST_CASE("paper ex 2.9: ns+r explores N executions (lazy ordering)", "[algo][dpor][paper]") {
  DporConfig config;
  std::set<std::string> first_receive_values;

  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 5, .value = "1"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 5, .value = "2"};
    }
    return std::nullopt;
  };
  config.program.threads[3] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 5, .value = "3"};
    }
    return std::nullopt;
  };
  config.program.threads[4] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 5, .value = "4"};
    }
    return std::nullopt;
  };
  config.program.threads[5] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.on_execution = [&first_receive_values](const ExplorationGraph& graph) {
    const auto trace = graph.thread_trace(5);
    if (!trace.empty()) {
      first_receive_values.insert(trace[0].value());
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 4);
  REQUIRE(first_receive_values == std::set<std::string>{"1", "2", "3", "4"});
  require_dpor_matches_oracle(config.program,
                              "T1=[S(5,1)]; T2=[S(5,2)]; T3=[S(5,3)]; T4=[S(5,4)]; T5=[Rb(*)]");
}

TEST_CASE("paper ex 4.1: blocked receive is rescheduled when sends appear", "[algo][dpor][paper]") {
  DporConfig config;
  std::set<std::string> observed_receive_values;
  bool completed_graph_has_block = false;

  // T3 (smallest tid) is scheduled first and tries to receive.
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  // Two sends to T3.
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "1"};
    }
    return std::nullopt;
  };
  config.program.threads[3] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "2"};
    }
    return std::nullopt;
  };

  config.on_execution = [&observed_receive_values,
                         &completed_graph_has_block](const ExplorationGraph& graph) {
    for (const auto& evt : graph.events()) {
      if (is_block(evt)) {
        completed_graph_has_block = true;
      }
    }
    const auto trace = graph.thread_trace(1);
    if (!trace.empty()) {
      observed_receive_values.insert(trace[0].value());
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_receive_values == std::set<std::string>{"1", "2"});
  REQUIRE_FALSE(completed_graph_has_block);
  require_dpor_matches_oracle(config.program, "T1=[Rb(*)]; T2=[S(1,1)]; T3=[S(1,2)]");
}

TEST_CASE("paper ex 4.2: backward revisit recovers missed rf option", "[algo][dpor][paper]") {
  DporConfig config;
  std::set<std::string> observed_receive_values;

  // Thread IDs arranged to force the paper's schedule shape:
  // T1 then T3(recv) then T2.
  // T1: send(T3,1)
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "1"};
    }
    return std::nullopt;
  };
  // T3: recv()
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  // T2: send(T3,2)
  config.program.threads[3] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "2"};
    }
    return std::nullopt;
  };

  config.on_execution = [&observed_receive_values](const ExplorationGraph& graph) {
    const auto trace = graph.thread_trace(2);
    if (!trace.empty()) {
      observed_receive_values.insert(trace[0].value());
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_receive_values == std::set<std::string>{"1", "2"});
  require_dpor_matches_oracle(config.program, "T1=[S(2,1)]; T2=[Rb(*)]; T3=[S(2,2)]");
}

TEST_CASE("fifo p2p prunes backward revisits that would skip an earlier same-sender send",
          "[algo][dpor][fifo_p2p][regression]") {
  Program program;

  program.threads[1] = [](const ThreadTrace& trace,
                          std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "a"};
    }
    if (step == 1 && trace.empty()) {
      return make_receive_label_from_values<Value>({"token"});
    }
    if (step == 2 && trace.size() == 1) {
      return SendLabel{.destination = 2, .value = "b"};
    }
    return std::nullopt;
  };
  program.threads[2] = [](const ThreadTrace& trace,
                          std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  program.threads[3] = [](const ThreadTrace&,
                          std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "token"};
    }
    return std::nullopt;
  };

  DporConfig async_config;
  async_config.program = program;
  async_config.communication_model = CommunicationModel::Async;

  DporConfig fifo_config;
  fifo_config.program = program;
  fifo_config.communication_model = CommunicationModel::FifoP2P;

  std::set<std::string> fifo_observed_receive_values;
  fifo_config.on_execution = [&fifo_observed_receive_values](const ExplorationGraph& graph) {
    const auto trace = graph.thread_trace(2);
    if (!trace.empty()) {
      fifo_observed_receive_values.insert(trace[0].value());
    }
  };

  const auto async_result = verify(async_config);
  const auto fifo_result = verify(fifo_config);

  REQUIRE(async_result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(async_result.executions_explored == 2);
  REQUIRE(fifo_result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(fifo_result.executions_explored == 1);
  REQUIRE(fifo_observed_receive_values == std::set<std::string>{"a"});
  require_dpor_matches_oracle(
      program, "fifo_p2p revisit prune: T1=[S(2,a),Rb({token}),S(2,b)]; T2=[Rb(*)]; T3=[S(1,token)]",
      CommunicationModel::FifoP2P);
}

TEST_CASE("paper ex 4.3: revisiting condition avoids duplicate exploration in s+s+r-br",
          "[algo][dpor][paper]") {
  DporConfig config;
  std::vector<std::string> signatures;

  // T1: send(T1,0); recv()
  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "0"};
    }
    if (step == 1 && trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  // T2: send(T4,1)
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "1"};
    }
    return std::nullopt;
  };
  // T3: send(T4,2)
  config.program.threads[3] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "2"};
    }
    return std::nullopt;
  };
  // T4: recv()
  config.program.threads[4] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  // T5: send(T1,42)
  config.program.threads[5] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "42"};
    }
    return std::nullopt;
  };

  config.on_execution = [&signatures](const ExplorationGraph& graph) {
    signatures.push_back(graph_signature(graph));
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 4);
  REQUIRE(signatures.size() == result.executions_explored);

  const std::set<std::string> unique_signatures(signatures.begin(), signatures.end());
  REQUIRE(unique_signatures.size() == signatures.size());
  require_dpor_matches_oracle(
      config.program, "T1=[S(1,0),Rb(*)]; T2=[S(4,1)]; T3=[S(4,2)]; T4=[Rb(*)]; T5=[S(1,42)]");
}

TEST_CASE("verify_parallel with one worker matches sequential execution order exactly",
          "[algo][dpor][parallel]") {
  const auto program = make_parallel_mixed_program();

  const auto sequential =
      collect_observed_executions(program, [](const DporConfig& config) { return verify(config); });

  const auto parallel = collect_observed_executions(program, [](const DporConfig& config) {
    ParallelVerifyOptions options;
    options.max_workers = 1;
    options.max_queued_tasks = 4;
    return verify_parallel(config, options);
  });

  REQUIRE(sequential.result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(parallel.result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(parallel.result.executions_explored == sequential.result.executions_explored);
  REQUIRE(parallel.unique == sequential.unique);
  REQUIRE(parallel.observed == sequential.observed);
}

TEST_CASE("try_enqueue_owned_task restores the graph after enqueue rejection",
          "[algo][dpor][parallel]") {
  ExplorationGraph graph;
  const auto send_id = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto recv_id =
      graph.add_event(2, make_receive_label_from_values<Value>({"x"}, ReceiveMode::NonBlocking));
  graph.set_reads_from(recv_id, send_id);
  const auto before = graph_signature(graph);

  RejectingEnqueueExecutor executor;
  REQUIRE_FALSE(dpor::algo::detail::try_enqueue_owned_task<Value>(
      executor, graph, 1, dpor::algo::detail::ExplorationTaskMode::VisitIfConsistent));
  REQUIRE(graph_signature(graph) == before);
}

TEST_CASE("verify_parallel matches sequential and oracle execution sets on mixed branching",
          "[algo][dpor][parallel]") {
  const auto program = make_parallel_mixed_program();
  const auto oracle = dpor::test_support::collect_oracle_stats(program);

  const auto sequential =
      collect_observed_executions(program, [](const DporConfig& config) { return verify(config); });

  const auto parallel = collect_observed_executions(program, [](const DporConfig& config) {
    ParallelVerifyOptions options;
    options.max_workers = 4;
    options.max_queued_tasks = 16;
    return verify_parallel(config, options);
  });

  REQUIRE(sequential.result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(parallel.result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(parallel.result.executions_explored == sequential.result.executions_explored);
  REQUIRE(parallel.unique == sequential.unique);
  REQUIRE(parallel.unique == oracle.signatures);
  REQUIRE(parallel.unique.size() == parallel.observed.size());
}

TEST_CASE("verify_parallel reports error terminals when sibling branches race to error",
          "[algo][dpor][parallel]") {
  Program program;
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "left",
          .choices = {"left", "right"},
      };
    }
    if (step == 1 && trace.size() == 1) {
      return ErrorLabel{.message = "parallel boom"};
    }
    return std::nullopt;
  };

  std::size_t observed_count = 0;
  bool saw_bad_error_graph = false;
  std::mutex observed_mutex;

  DporConfig config;
  config.program = program;
  config.on_execution = [&](const ExplorationGraph& graph) {
    std::lock_guard lock(observed_mutex);
    ++observed_count;
    if (graph.event_count() != 2 || !is_error(graph.event(1))) {
      saw_bad_error_graph = true;
    }
  };

  ParallelVerifyOptions options;
  options.max_workers = 2;
  options.max_queued_tasks = 4;

  const auto result = verify_parallel(config, options);
  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.full_executions_explored == 0);
  REQUIRE(result.error_executions_explored == 2);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_count == 2);
  REQUIRE_FALSE(saw_bad_error_graph);
}

TEST_CASE("verify_parallel can stop when terminal observer requests stop",
          "[algo][dpor][parallel]") {
  Program program;
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "left",
          .choices = {"left", "right"},
      };
    }
    return std::nullopt;
  };

  std::size_t observed_count = 0;
  std::mutex observed_mutex;

  DporConfig config;
  config.program = program;
  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    std::lock_guard lock(observed_mutex);
    REQUIRE(execution.kind == TerminalExecutionKind::Full);
    ++observed_count;
    return TerminalExecutionAction::Stop;
  };

  ParallelVerifyOptions options;
  options.max_workers = 2;
  options.max_queued_tasks = 4;

  const auto result = verify_parallel(config, options);
  REQUIRE(result.kind == VerifyResultKind::Stopped);
  REQUIRE(result.full_executions_explored >= 1);
  REQUIRE(result.full_executions_explored <= 2);
  REQUIRE(result.error_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.executions_explored == result.full_executions_explored);
  REQUIRE(observed_count >= 1);
  REQUIRE(observed_count <= 2);
}

TEST_CASE("verify_parallel reports approximate running progress and exact final progress",
          "[algo][dpor][parallel]") {
  Program program;
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "left",
          .choices = {"left", "right", "up", "down"},
      };
    }
    return std::nullopt;
  };

  std::vector<ProgressSnapshot> snapshots;
  std::mutex snapshots_mutex;

  DporConfig config;
  config.program = program;
  config.progress_report_interval = std::chrono::milliseconds::zero();
  config.on_progress = [&](const ProgressSnapshot& snapshot) {
    std::lock_guard lock(snapshots_mutex);
    snapshots.push_back(snapshot);
  };

  ParallelVerifyOptions options;
  options.max_workers = 2;
  options.max_queued_tasks = 4;
  options.progress_counter_flush_interval = 1000;

  const auto result = verify_parallel(config, options);

  REQUIRE_FALSE(snapshots.empty());
  bool saw_running = false;
  bool saw_inexact_running = false;
  {
    std::lock_guard lock(snapshots_mutex);
    for (const auto& snapshot : snapshots) {
      if (snapshot.state != ProgressState::Running) {
        continue;
      }
      saw_running = true;
      REQUIRE(snapshot.max_workers == 2);
      REQUIRE(snapshot.max_queued_tasks == 4);
      if (!snapshot.counts_exact) {
        saw_inexact_running = true;
      }
    }

    REQUIRE(saw_running);
    REQUIRE(saw_inexact_running);
    const auto& final_snapshot = snapshots.back();
    REQUIRE(final_snapshot.state == ProgressState::AllExplored);
    REQUIRE(final_snapshot.counts_exact);
    REQUIRE(final_snapshot.terminal_executions == result.executions_explored);
    REQUIRE(final_snapshot.full_executions == result.full_executions_explored);
    REQUIRE(final_snapshot.error_executions == result.error_executions_explored);
    REQUIRE(final_snapshot.depth_limit_executions == result.depth_limit_executions_explored);
    REQUIRE(final_snapshot.active_workers == 0);
    REQUIRE(final_snapshot.max_workers == 2);
    REQUIRE(final_snapshot.queued_tasks == 0);
    REQUIRE(final_snapshot.max_queued_tasks == 4);
  }
}

TEST_CASE("verify_parallel reports depth-limit terminals when one branch exceeds max_depth",
          "[algo][dpor][parallel]") {
  Program program;
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "done",
          .choices = {"done", "loop"},
      };
    }
    if (trace.size() != 1) {
      return std::nullopt;
    }
    if (trace[0].value() == "done") {
      return std::nullopt;
    }
    return SendLabel{.destination = 2, .value = "tick"};
  };

  std::size_t observed_count = 0;
  std::size_t full_observed_count = 0;
  std::size_t depth_limit_observed_count = 0;
  std::mutex observed_mutex;

  DporConfig config;
  config.program = program;
  config.max_depth = 2;
  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    std::lock_guard lock(observed_mutex);
    ++observed_count;
    if (execution.kind == TerminalExecutionKind::Full) {
      ++full_observed_count;
    } else if (execution.kind == TerminalExecutionKind::DepthLimit) {
      ++depth_limit_observed_count;
    }
  };

  ParallelVerifyOptions options;
  options.max_workers = 2;
  options.max_queued_tasks = 4;

  const auto result = verify_parallel(config, options);
  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.full_executions_explored == 1);
  REQUIRE(result.error_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 1);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_count == 2);
  REQUIRE(full_observed_count == 1);
  REQUIRE(depth_limit_observed_count == 1);
}

TEST_CASE("verify_parallel matches sequential under tiny queue budget and high fanout",
          "[algo][dpor][parallel]") {
  Program program;
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b", "c", "d", "e"},
      };
    }
    if (step == 1 && trace.size() == 1) {
      return NondeterministicChoiceLabel{
          .value = "u",
          .choices = {"u", "v", "w"},
      };
    }
    return std::nullopt;
  };

  const auto oracle = dpor::test_support::collect_oracle_stats(program);
  const auto sequential =
      collect_observed_executions(program, [](const DporConfig& config) { return verify(config); });

  const auto parallel = collect_observed_executions(program, [](const DporConfig& config) {
    ParallelVerifyOptions options;
    options.max_workers = 2;
    options.max_queued_tasks = 1;
    return verify_parallel(config, options);
  });

  REQUIRE(sequential.result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(parallel.result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(parallel.result.executions_explored == 15);
  REQUIRE(parallel.result.executions_explored == sequential.result.executions_explored);
  REQUIRE(parallel.unique == sequential.unique);
  REQUIRE(parallel.unique == oracle.signatures);
  REQUIRE(parallel.unique.size() == parallel.observed.size());
}

TEST_CASE("verify handles a deep linear execution without recursive stack growth",
          "[algo][dpor][stack]") {
  constexpr std::size_t kDepth = 12000;

  Program program;
  program.threads[1] = [](const ThreadTrace&, const std::size_t step) -> std::optional<EventLabel> {
    if (step < kDepth) {
      return SendLabel{.destination = 1, .value = "tick"};
    }
    return std::nullopt;
  };

  DporConfig config;
  config.program = program;
  config.max_depth = kDepth + 1U;

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.full_executions_explored == 1);
  REQUIRE(result.error_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.executions_explored == 1);
}

TEST_CASE("verify_parallel with one worker handles a deep linear execution without recursive stack growth",
          "[algo][dpor][parallel][stack]") {
  constexpr std::size_t kDepth = 12000;

  Program program;
  program.threads[1] = [](const ThreadTrace&, const std::size_t step) -> std::optional<EventLabel> {
    if (step < kDepth) {
      return SendLabel{.destination = 1, .value = "tick"};
    }
    return std::nullopt;
  };

  DporConfig config;
  config.program = program;
  config.max_depth = kDepth + 1U;

  ParallelVerifyOptions options;
  options.max_workers = 1;
  options.max_queued_tasks = 4;

  const auto result = verify_parallel(config, options);
  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.full_executions_explored == 1);
  REQUIRE(result.error_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.executions_explored == 1);
}
