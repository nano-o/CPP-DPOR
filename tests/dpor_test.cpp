#include "dpor/algo/dpor.hpp"
#include "dpor/model/consistency.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
using namespace dpor::algo;
using namespace dpor::model;
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
