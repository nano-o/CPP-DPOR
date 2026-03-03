#include "dpor/algo/dpor.hpp"
#include "dpor/model/consistency.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace dpor::algo;
using namespace dpor::model;

std::string event_signature(const Event& event) {
  std::ostringstream oss;
  oss << "t" << event.thread << ":i" << event.index << ":";
  if (const auto* send = as_send(event)) {
    oss << "S(dst=" << send->destination << ",v=" << send->value << ")";
  } else if (const auto* nd = as_nondeterministic_choice(event)) {
    oss << "ND(v=" << nd->value << ")";
  } else if (is_receive(event)) {
    oss << "R";
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
  for (const auto& [recv_id, send_id] : graph.reads_from()) {
    rf_edges.push_back(
        event_signature(graph.event(send_id)) + "->" + event_signature(graph.event(recv_id)));
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

ExplorationGraph::EventId find_event_id_by_thread_index(
    const ExplorationGraph& graph,
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
}  // namespace

// --- Empty and trivial programs ---

TEST_CASE("empty program explores 1 execution", "[algo][dpor]") {
  DporConfig config;
  config.program.threads = {};

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
}

TEST_CASE("single thread with one send explores 1 execution", "[algo][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
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
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
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
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "x"};
    }
    return std::nullopt;
  };

  // Thread 2: receive (match any).
  config.program.threads[2] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
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

TEST_CASE("error event results in ErrorFound", "[algo][dpor]") {
  DporConfig config;

  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return ErrorLabel{};
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::ErrorFound);
  REQUIRE(result.executions_explored == 1);
}

// --- ND choices ---

TEST_CASE("ND choice with 2 options explores 2 executions", "[algo][dpor]") {
  DporConfig config;

  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
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
}

TEST_CASE("ND choice with 3 options explores 3 executions", "[algo][dpor]") {
  DporConfig config;

  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
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
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "a"};
    }
    return std::nullopt;
  };

  // Thread 2: send "b" to thread 3.
  config.program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "b"};
    }
    return std::nullopt;
  };

  // Thread 3: receive one message (match any).
  config.program.threads[3] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
}

// --- s+s+r example (paper Example 2.3) ---

TEST_CASE("s+s+r: two sends from one thread, one receive, explores 2 executions", "[algo][dpor]") {
  DporConfig config;

  // Thread 1: send "a" then send "b" to thread 2.
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "a"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "b"};
    }
    return std::nullopt;
  };

  // Thread 2: receive one message (match any).
  config.program.threads[2] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
}

TEST_CASE("receiver-first schedule still explores both rf choices via backward revisit",
    "[algo][dpor][regression]") {
  DporConfig config;

  // T1 has the smallest tid and is considered first by next-event selection.
  // It performs a receive as its first operation.
  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  // Two independent senders to T1.
  config.program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "a"};
    }
    return std::nullopt;
  };

  config.program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "b"};
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  // One execution where T1 reads from T2's send and one from T3's send.
  REQUIRE(result.executions_explored == 2);
}

TEST_CASE("backward-revisit-heavy exploration does not produce duplicate execution graphs",
    "[algo][dpor][regression]") {
  DporConfig config;
  std::vector<std::string> signatures;

  // Receiver thread (smallest tid) performs two receives.
  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step < 2 && trace.size() == step) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "a"};
    }
    return std::nullopt;
  };

  config.program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "b"};
    }
    return std::nullopt;
  };

  config.program.threads[4] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
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

  // Thread that sends indefinitely.
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return SendLabel{.destination = 2, .value = "x"};
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  // With max_depth=2, it should stop early rather than looping forever.
}

// --- Execution observer ---

TEST_CASE("execution observer is called for each complete execution", "[algo][dpor]") {
  DporConfig config;
  std::size_t observed_count = 0;

  config.on_execution = [&observed_count](const ExplorationGraph&) {
    ++observed_count;
  };

  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b"},
      };
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_count == 2);
}

// --- Cycle-inducing rf pruned ---

TEST_CASE("cycle-inducing rf assignment is pruned by consistency check", "[algo][dpor]") {
  DporConfig config;

  // Thread 1: send to thread 2, then receive from thread 2.
  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "a"};
    }
    if (step == 1) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  // Thread 2: send to thread 1, then receive from thread 1.
  config.program.threads[2] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
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
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "x"};
    }
    return std::nullopt;
  };

  // Thread 2: receive then send to thread 3.
  config.program.threads[2] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    if (step == 1 && trace.size() == 1) {
      return SendLabel{.destination = 3, .value = trace[0]};
    }
    return std::nullopt;
  };

  // Thread 3: receive.
  config.program.threads[3] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
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

TEST_CASE("block event terminates the thread", "[algo][dpor]") {
  DporConfig config;
  std::size_t send_count = 0;

  config.on_execution = [&send_count](const ExplorationGraph& g) {
    for (const auto& evt : g.events()) {
      if (is_send(evt)) {
        ++send_count;
      }
    }
  };

  // Thread returns block at step 0, then would return a send at step 1 —
  // but the engine should never ask for step 1 because block terminates the thread.
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return BlockLabel{};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "x"};
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  // The send after block should never be produced.
  REQUIRE(send_count == 0);
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

  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b"},
      };
    }
    if (step == 1 && trace.size() == 1) {
      // Send the ND choice value.
      return SendLabel{.destination = 2, .value = trace[0]};
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
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  // T2: S(3,c); S(1,a)
  config.program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "c"};
    }
    if (step == 1) {
      return SendLabel{.destination = 1, .value = "a"};
    }
    return std::nullopt;
  };

  // T3: R; S(1,b)
  config.program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    if (step == 1) {
      return SendLabel{.destination = 1, .value = "b"};
    }
    return std::nullopt;
  };

  // T4: ND{b,a}; S(3,b)
  config.program.threads[4] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
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
  static_cast<void>(graph.add_event(
      4,
      NondeterministicChoiceLabel{
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
  dpor::algo::detail::backward_revisit(
      config.program, graph, s41, result, config, 0);

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
  dpor::algo::detail::backward_revisit(config.program, graph, send, result, config, 0);
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
  const auto nd_bad = graph.add_event(
      6,
      NondeterministicChoiceLabel{
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
  dpor::algo::detail::backward_revisit(config.program, graph, s41, result, config, 0);
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

  dpor::algo::detail::backward_revisit(config.program, graph, s_new, result, config, 0);

  REQUIRE(result.executions_explored == 1);
  REQUIRE(revisited_graphs.size() == 1);

  const auto& revisited = revisited_graphs.front();
  REQUIRE(revisited.event_count() == 3);
  REQUIRE(find_event_id_by_thread_index(revisited, 3, 0) == ExplorationGraph::kNoSource);

  const auto new_receive_id = find_event_id_by_thread_index(revisited, 4, 0);
  REQUIRE(new_receive_id != ExplorationGraph::kNoSource);

  const auto rf_it = revisited.reads_from().find(new_receive_id);
  REQUIRE(rf_it != revisited.reads_from().end());

  const auto* rf_send = as_send(revisited.event(rf_it->second));
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

  dpor::algo::detail::backward_revisit(config.program, graph, s_new, result, config, 0);

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

  const auto* rb_send = as_send(revisited.event(rb_rf->second));
  const auto* ra_send = as_send(revisited.event(ra_rf->second));
  REQUIRE(rb_send != nullptr);
  REQUIRE(ra_send != nullptr);

  REQUIRE(rb_send->value == "b");
  REQUIRE(rb_send->destination == 4);
  REQUIRE(ra_send->value == "a");
  REQUIRE(ra_send->destination == 4);
  REQUIRE(revisited.event(ra_rf->second).thread == 3);
}

TEST_CASE("tiebreaker should not pick an already-consumed send", "[algo][dpor][regression]") {
  ExplorationGraph graph;

  const auto s1 = graph.add_event(1, SendLabel{.destination = 3, .value = "x"});
  const auto r0 = graph.add_event(3, make_receive_label<Value>());
  const auto s2 = graph.add_event(2, SendLabel{.destination = 3, .value = "y"});
  const auto r1 = graph.add_event(3, make_receive_label<Value>());

  graph.set_reads_from(r0, s1);  // s1 already consumed

  const auto chosen = dpor::algo::detail::get_cons_tiebreaker(graph, r1);
  REQUIRE(chosen == s2);
}

TEST_CASE("ND revisit condition should use min(S), not insertion order", "[algo][dpor][regression]") {
  ExplorationGraph graph;
  const auto nd = graph.add_event(
      1,
      NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"b", "a"},
      });
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});

  // Under the paper's condition val(e) = min(S), this should hold.
  REQUIRE(dpor::algo::detail::revisit_condition(graph, nd, s));
}

// --- Paper examples (Must, OOPSLA'24) within current async + ND scope ---

TEST_CASE("paper ex 2.4: nondet failure target yields one execution per choice", "[algo][dpor][paper]") {
  DporConfig config;
  std::set<std::string> received_failures;

  // T1 (environment): who := nondet({node1,node2}); send(T2, Fail(who))
  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "node1",
          .choices = {"node1", "node2"},
      };
    }
    if (step == 1 && trace.size() == 1) {
      return SendLabel{.destination = 2, .value = "Fail(" + trace[0] + ")"};
    }
    return std::nullopt;
  };

  // T2 (coordinator): receive one failure notification.
  config.program.threads[2] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.on_execution = [&received_failures](const ExplorationGraph& graph) {
    const auto trace = graph.thread_trace(2);
    if (!trace.empty()) {
      received_failures.insert(trace[0]);
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(received_failures == std::set<std::string>{"Fail(node1)", "Fail(node2)"});
}

TEST_CASE("paper ex 2.6: selective receive filters stale value", "[algo][dpor][paper]") {
  DporConfig config;

  // Two messages to T3: one stale, one fresh.
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "stale"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "fresh"};
    }
    return std::nullopt;
  };

  // T3 only accepts "fresh".
  config.program.threads[3] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label_from_values<Value>({"fresh"});
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
}

TEST_CASE("paper ex 2.7: ordered selective receives collapse ns+rn-sel to one execution",
    "[algo][dpor][paper]") {
  DporConfig config;

  // T1..T3 send 1..3 to T4.
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "1"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "2"};
    }
    return std::nullopt;
  };
  config.program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "3"};
    }
    return std::nullopt;
  };

  // T4 receives exactly 1, then 2, then 3.
  config.program.threads[4] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
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
}

TEST_CASE("paper ex 2.8: receives can consume messages out of sender order with predicates",
    "[algo][dpor][paper]") {
  DporConfig config;

  // T1: send(T2,1); send(T2,2)
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "1"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "2"};
    }
    return std::nullopt;
  };

  // T2: recv(x==2); recv(x==1)
  config.program.threads[2] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
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
}

TEST_CASE("paper ex 2.9: ns+r explores N executions (lazy ordering)", "[algo][dpor][paper]") {
  DporConfig config;
  std::set<std::string> first_receive_values;

  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 5, .value = "1"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 5, .value = "2"};
    }
    return std::nullopt;
  };
  config.program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 5, .value = "3"};
    }
    return std::nullopt;
  };
  config.program.threads[4] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 5, .value = "4"};
    }
    return std::nullopt;
  };
  config.program.threads[5] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  config.on_execution = [&first_receive_values](const ExplorationGraph& graph) {
    const auto trace = graph.thread_trace(5);
    if (!trace.empty()) {
      first_receive_values.insert(trace[0]);
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 4);
  REQUIRE(first_receive_values == std::set<std::string>{"1", "2", "3", "4"});
}

TEST_CASE("paper ex 4.2: backward revisit recovers missed rf option", "[algo][dpor][paper]") {
  DporConfig config;
  std::set<std::string> observed_receive_values;

  // Thread IDs arranged to force the paper's schedule shape:
  // T1 then T3(recv) then T2.
  // T1: send(T3,1)
  config.program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "1"};
    }
    return std::nullopt;
  };
  // T3: recv()
  config.program.threads[2] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  // T2: send(T3,2)
  config.program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "2"};
    }
    return std::nullopt;
  };

  config.on_execution = [&observed_receive_values](const ExplorationGraph& graph) {
    const auto trace = graph.thread_trace(2);
    if (!trace.empty()) {
      observed_receive_values.insert(trace[0]);
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(observed_receive_values == std::set<std::string>{"1", "2"});
}

TEST_CASE("paper ex 4.3: revisiting condition avoids duplicate exploration in s+s+r-br",
    "[algo][dpor][paper]") {
  DporConfig config;
  std::vector<std::string> signatures;

  // T1: send(T1,0); recv()
  config.program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "0"};
    }
    if (step == 1 && trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  // T2: send(T4,1)
  config.program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "1"};
    }
    return std::nullopt;
  };
  // T3: send(T4,2)
  config.program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 4, .value = "2"};
    }
    return std::nullopt;
  };
  // T4: recv()
  config.program.threads[4] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  // T5: send(T1,42)
  config.program.threads[5] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
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
}
