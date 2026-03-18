#include "dpor/model/consistency.hpp"

#include "dpor/model/execution_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
using namespace dpor::model;

bool has_issue(const ConsistencyResult& result, const ConsistencyIssueCode code) {
  return std::any_of(result.issues.begin(), result.issues.end(),
                     [code](const ConsistencyIssue& issue) { return issue.code == code; });
}

std::size_t count_issues(const ConsistencyResult& result, const ConsistencyIssueCode code) {
  return static_cast<std::size_t>(
      std::count_if(result.issues.begin(), result.issues.end(),
                    [code](const ConsistencyIssue& issue) { return issue.code == code; }));
}

std::vector<ConsistencyIssueCode> issue_codes(const ConsistencyResult& result) {
  std::vector<ConsistencyIssueCode> codes;
  codes.reserve(result.issues.size());
  for (const auto& issue : result.issues) {
    codes.push_back(issue.code);
  }
  return codes;
}
}  // namespace

// --- ConsistencyResult unit tests ---

TEST_CASE("ConsistencyResult::success creates empty result", "[model][consistency]") {
  const auto result = ConsistencyResult::success();
  REQUIRE(result.is_consistent());
  REQUIRE(result.issues.empty());
}

TEST_CASE("ConsistencyResult::failure creates single-issue result", "[model][consistency]") {
  const auto result =
      ConsistencyResult::failure(ConsistencyIssueCode::CausalCycle, "cycle detected");
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(result.issues.size() == 1);
  REQUIRE(result.issues[0].code == ConsistencyIssueCode::CausalCycle);
  REQUIRE(result.issues[0].message == "cycle detected");
}

// --- Empty and trivial graphs ---

TEST_CASE("empty graph is consistent", "[model][consistency]") {
  ExecutionGraph graph;
  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("graph with only sends (no receives) is consistent", "[model][consistency]") {
  ExecutionGraph graph;
  static_cast<void>(graph.add_event(1, SendLabel{.destination = 2, .value = "a"}));
  static_cast<void>(graph.add_event(1, SendLabel{.destination = 3, .value = "b"}));
  static_cast<void>(graph.add_event(2, SendLabel{.destination = 1, .value = "c"}));

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("graph with non-communication events is consistent", "[model][consistency]") {
  ExecutionGraph graph;
  static_cast<void>(graph.add_event(1, BlockLabel{}));
  static_cast<void>(graph.add_event(2, ErrorLabel{}));

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

// --- Well-formed graphs ---

TEST_CASE("simple well-formed send-receive pair is consistent", "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "ok"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"ok"}));
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("multiple independent send-receive pairs are consistent", "[model][consistency]") {
  ExecutionGraph graph;
  const auto s0 = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r0 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  const auto s1 = graph.add_event(3, SendLabel{.destination = 4, .value = "y"});
  const auto r1 = graph.add_event(4, make_receive_label_from_values<Value>({"y"}));

  graph.set_reads_from(r0, s0);
  graph.set_reads_from(r1, s1);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("chain: thread 1 sends to 2, thread 2 sends to 3 (acyclic)", "[model][consistency]") {
  ExecutionGraph graph;

  const auto s1 = graph.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto r2 = graph.add_event(2, make_receive_label_from_values<Value>({"a"}));
  const auto s2 = graph.add_event(2, SendLabel{.destination = 3, .value = "b"});
  const auto r3 = graph.add_event(3, make_receive_label_from_values<Value>({"b"}));

  graph.set_reads_from(r2, s1);
  graph.set_reads_from(r3, s2);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("receive using match-any predicate is consistent", "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "anything"});
  const auto r = graph.add_event(2, make_receive_label<Value>());
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("non-blocking receive reading bottom is consistent", "[model][consistency]") {
  ExecutionGraph graph;
  const auto r = graph.add_event(2, make_nonblocking_receive_label<Value>());
  graph.set_reads_from_bottom(r);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("blocking receive reading bottom reports BlockingReceiveReadsBottom",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto r = graph.add_event(2, make_receive_label<Value>());
  graph.set_reads_from_bottom(r);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::BlockingReceiveReadsBottom));
}

// --- InvalidEventReference ---

TEST_CASE("reads-from with invalid receive id reports InvalidEventReference",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  graph.set_reads_from(999, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::InvalidEventReference));
}

TEST_CASE("reads-from with invalid source id reports InvalidEventReference",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(r, 999);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::InvalidEventReference));
}

TEST_CASE("reads-from with both ids invalid reports two InvalidEventReference issues",
          "[model][consistency]") {
  ExecutionGraph graph;
  static_cast<void>(graph.add_event(1, SendLabel{.destination = 2, .value = "x"}));
  graph.set_reads_from(888, 999);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(count_issues(result, ConsistencyIssueCode::InvalidEventReference) == 2);
}

// --- ReadsFromTargetNotReceive ---

TEST_CASE("reads-from target that is a send reports ReadsFromTargetNotReceive",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s0 = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto s1 = graph.add_event(2, SendLabel{.destination = 1, .value = "y"});
  graph.set_reads_from(s0, s1);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReadsFromTargetNotReceive));
}

TEST_CASE("reads-from target that is a block reports ReadsFromTargetNotReceive",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto b = graph.add_event(1, BlockLabel{});
  const auto s = graph.add_event(2, SendLabel{.destination = 1, .value = "x"});
  graph.set_reads_from(b, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReadsFromTargetNotReceive));
}

// --- ReadsFromSourceNotSend ---

TEST_CASE("reads-from source that is a receive reports ReadsFromSourceNotSend",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto r0 = graph.add_event(1, make_receive_label_from_values<Value>({"x"}));
  const auto r1 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(r0, r1);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReadsFromSourceNotSend));
}

TEST_CASE("reads-from source that is error reports ReadsFromSourceNotSend",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto e = graph.add_event(1, ErrorLabel{});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(r, e);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReadsFromSourceNotSend));
}

// --- MissingReadsFromForReceive ---

TEST_CASE("receive without reads-from source reports MissingReadsFromForReceive",
          "[model][consistency]") {
  ExecutionGraph graph;
  static_cast<void>(graph.add_event(1, make_receive_label_from_values<Value>({"x"})));

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::MissingReadsFromForReceive));
}

TEST_CASE("multiple receives without sources each report MissingReadsFromForReceive",
          "[model][consistency]") {
  ExecutionGraph graph;
  static_cast<void>(graph.add_event(1, make_receive_label_from_values<Value>({"x"})));
  static_cast<void>(graph.add_event(2, make_receive_label_from_values<Value>({"y"})));

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(count_issues(result, ConsistencyIssueCode::MissingReadsFromForReceive) == 2);
}

TEST_CASE("one receive matched, one unmatched: only the unmatched reports missing source",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r0 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  static_cast<void>(graph.add_event(3, make_receive_label_from_values<Value>({"y"})));

  graph.set_reads_from(r0, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(count_issues(result, ConsistencyIssueCode::MissingReadsFromForReceive) == 1);
}

// --- SendConsumedMultipleTimes ---

TEST_CASE("one send consumed by two receives reports SendConsumedMultipleTimes",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r0 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  const auto r1 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));

  graph.set_reads_from(r0, s);
  graph.set_reads_from(r1, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::SendConsumedMultipleTimes));
}

TEST_CASE("one send consumed by three receives reports SendConsumedMultipleTimes twice",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r0 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  const auto r1 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  const auto r2 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));

  graph.set_reads_from(r0, s);
  graph.set_reads_from(r1, s);
  graph.set_reads_from(r2, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(count_issues(result, ConsistencyIssueCode::SendConsumedMultipleTimes) == 2);
}

// --- ReceiveDestinationMismatch ---

TEST_CASE("receive in wrong thread reports ReceiveDestinationMismatch", "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 5, .value = "x"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReceiveDestinationMismatch));
}

TEST_CASE("no ReceiveDestinationMismatch when send targets the receive thread",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(has_issue(result, ConsistencyIssueCode::ReceiveDestinationMismatch));
}

// --- ReceiveValueMismatch ---

TEST_CASE("receive that rejects sent value reports ReceiveValueMismatch", "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "unexpected"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"expected"}));
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReceiveValueMismatch));
}

TEST_CASE("receive with custom predicate that rejects reports ReceiveValueMismatch",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "short"});
  const auto r =
      graph.add_event(2, make_receive_label<Value>([](const Value& v) { return v.size() > 10; }));
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReceiveValueMismatch));
}

TEST_CASE("receive with custom predicate that accepts is consistent", "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "long enough string"});
  const auto r =
      graph.add_event(2, make_receive_label<Value>([](const Value& v) { return v.size() > 10; }));
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

// --- CausalCycle ---

TEST_CASE("two-thread cycle: recv before send on both threads", "[model][consistency]") {
  ExecutionGraph graph;

  const auto r1 = graph.add_event_with_index(1, 0, make_receive_label_from_values<Value>({"b"}));
  const auto s1 = graph.add_event_with_index(1, 1, SendLabel{.destination = 2, .value = "a"});
  const auto r2 = graph.add_event_with_index(2, 0, make_receive_label_from_values<Value>({"a"}));
  const auto s2 = graph.add_event_with_index(2, 1, SendLabel{.destination = 1, .value = "b"});

  graph.set_reads_from(r1, s2);
  graph.set_reads_from(r2, s1);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::CausalCycle));
}

TEST_CASE("three-thread cycle: A->B->C->A forms causal cycle", "[model][consistency]") {
  ExecutionGraph graph;

  // Thread 1: recv from thread 3, then send to thread 2
  const auto r1 = graph.add_event_with_index(1, 0, make_receive_label_from_values<Value>({"c"}));
  const auto s1 = graph.add_event_with_index(1, 1, SendLabel{.destination = 2, .value = "a"});

  // Thread 2: recv from thread 1, then send to thread 3
  const auto r2 = graph.add_event_with_index(2, 0, make_receive_label_from_values<Value>({"a"}));
  const auto s2 = graph.add_event_with_index(2, 1, SendLabel{.destination = 3, .value = "b"});

  // Thread 3: recv from thread 2, then send to thread 1
  const auto r3 = graph.add_event_with_index(3, 0, make_receive_label_from_values<Value>({"b"}));
  const auto s3 = graph.add_event_with_index(3, 1, SendLabel{.destination = 1, .value = "c"});

  graph.set_reads_from(r1, s3);
  graph.set_reads_from(r2, s1);
  graph.set_reads_from(r3, s2);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::CausalCycle));
}

TEST_CASE("acyclic cross-thread communication: send before recv on both threads",
          "[model][consistency]") {
  ExecutionGraph graph;

  // Thread 1: send to 2, then recv from 2
  const auto s1 = graph.add_event_with_index(1, 0, SendLabel{.destination = 2, .value = "a"});
  const auto r1 = graph.add_event_with_index(1, 1, make_receive_label_from_values<Value>({"b"}));

  // Thread 2: recv from 1 (program-order first), then send to 1
  const auto r2 = graph.add_event_with_index(2, 0, make_receive_label_from_values<Value>({"a"}));
  const auto s2 = graph.add_event_with_index(2, 1, SendLabel{.destination = 1, .value = "b"});

  graph.set_reads_from(r1, s2);
  graph.set_reads_from(r2, s1);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  // s1 (1,0) -rf-> r2 (2,0) -po-> s2 (2,1) -rf-> r1 (1,1): no cycle
  REQUIRE(result.is_consistent());
}

TEST_CASE("self-loop: send reads-from itself reports target-not-receive", "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 1, .value = "x"});
  graph.set_reads_from(s, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReadsFromTargetNotReceive));
  // Source IS a send, so ReadsFromSourceNotSend is not reported.
  // But endpoint kind check fails, so no further destination/value checks.
  REQUIRE_FALSE(has_issue(result, ConsistencyIssueCode::ReadsFromSourceNotSend));
}

// --- Multiple simultaneous issues ---

TEST_CASE("graph with destination mismatch AND value mismatch reports both",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 5, .value = "wrong"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"right"}));
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReceiveDestinationMismatch));
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReceiveValueMismatch));
}

TEST_CASE("invalid references skip further endpoint checks for that edge", "[model][consistency]") {
  ExecutionGraph graph;
  static_cast<void>(graph.add_event(1, SendLabel{.destination = 2, .value = "x"}));
  graph.set_reads_from(500, 600);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::InvalidEventReference));
  // Should not report other types of issues for the invalid-reference edge
  REQUIRE_FALSE(has_issue(result, ConsistencyIssueCode::ReadsFromTargetNotReceive));
  REQUIRE_FALSE(has_issue(result, ConsistencyIssueCode::ReadsFromSourceNotSend));
}

TEST_CASE("wrong endpoint kinds skip destination and value checks for that edge",
          "[model][consistency]") {
  ExecutionGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));

  // send -> send: target is not a receive, source is valid
  graph.set_reads_from(s, s);
  // receive reads from receive: source is not a send
  graph.set_reads_from(r, r);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReadsFromTargetNotReceive));
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReadsFromSourceNotSend));
  // Should not report destination/value mismatch since endpoint kind check failed
  REQUIRE_FALSE(has_issue(result, ConsistencyIssueCode::ReceiveDestinationMismatch));
  REQUIRE_FALSE(has_issue(result, ConsistencyIssueCode::ReceiveValueMismatch));
}

TEST_CASE("exploration-graph checker overload preserves malformed rf diagnostics",
          "[model][consistency][exploration_graph]") {
  ExplorationGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  graph.set_reads_from(12345, s);

  const AsyncConsistencyChecker checker;
  const auto execution_result = checker.check(graph.execution_graph());
  const auto exploration_result = checker.check(graph);

  REQUIRE(issue_codes(exploration_result) == issue_codes(execution_result));
  REQUIRE(has_issue(exploration_result, ConsistencyIssueCode::InvalidEventReference));
  REQUIRE_FALSE(has_issue(exploration_result, ConsistencyIssueCode::CausalCycle));
}

TEST_CASE(
    "exploration-graph checker overload still reports cycle alongside malformed rf diagnostics",
    "[model][consistency][exploration_graph]") {
  ExplorationGraph graph;

  const auto r1 = graph.add_event(1, make_receive_label<Value>());
  const auto s1 = graph.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto r2 = graph.add_event(2, make_receive_label<Value>());
  const auto s2 = graph.add_event(2, SendLabel{.destination = 1, .value = "b"});

  graph.set_reads_from(r1, s2);
  graph.set_reads_from(r2, s1);
  graph.set_reads_from(12345, s1);

  const AsyncConsistencyChecker checker;
  const auto execution_result = checker.check(graph.execution_graph());
  const auto exploration_result = checker.check(graph);

  REQUIRE(issue_codes(exploration_result) == issue_codes(execution_result));
  REQUIRE(has_issue(exploration_result, ConsistencyIssueCode::InvalidEventReference));
  REQUIRE(has_issue(exploration_result, ConsistencyIssueCode::CausalCycle));
}

TEST_CASE("exploration-graph checker overload still reports cycle with non-structural issues",
          "[model][consistency][exploration_graph]") {
  ExplorationGraph graph;

  const auto r1 = graph.add_event(1, make_receive_label_from_values<Value>({"b"}));
  const auto s1 = graph.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto r2 = graph.add_event(2, make_receive_label_from_values<Value>({"a"}));
  const auto s2 = graph.add_event(2, SendLabel{.destination = 1, .value = "b"});
  static_cast<void>(graph.add_event(3, make_receive_label_from_values<Value>({"c"})));

  graph.set_reads_from(r1, s2);
  graph.set_reads_from(r2, s1);

  const AsyncConsistencyChecker checker;
  const auto execution_result = checker.check(graph.execution_graph());
  const auto exploration_result = checker.check(graph);

  REQUIRE(issue_codes(exploration_result) == issue_codes(execution_result));
  REQUIRE(has_issue(exploration_result, ConsistencyIssueCode::MissingReadsFromForReceive));
  REQUIRE(has_issue(exploration_result, ConsistencyIssueCode::CausalCycle));
}

TEST_CASE("exploration-graph checker overload leaves cold porf cache untouched",
          "[model][consistency][exploration_graph]") {
  ExplorationGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(r, s);

  REQUIRE(graph.is_known_acyclic());
  REQUIRE_FALSE(graph.has_porf_cache());

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE(result.is_consistent());
  REQUIRE_FALSE(graph.has_porf_cache());
}

TEST_CASE("exploration-graph checker overload reuses warm porf cache",
          "[model][consistency][exploration_graph]") {
  ExplorationGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(r, s);

  REQUIRE_FALSE(graph.has_porf_cache());
  REQUIRE(graph.porf_contains(s, r));
  REQUIRE(graph.has_porf_cache());

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE(result.is_consistent());
  REQUIRE(graph.has_porf_cache());
}

TEST_CASE("exploration-graph checker re-arms known acyclicity for with_rf copies",
          "[model][consistency][exploration_graph]") {
  ExplorationGraph graph;
  const auto s1 = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r1 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  graph.set_reads_from(r1, s1);

  auto revisited = graph.with_rf(r1, s1);
  REQUIRE_FALSE(revisited.is_known_acyclic());
  REQUIRE_FALSE(revisited.has_porf_cache());

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(revisited);

  REQUIRE(result.is_consistent());
  REQUIRE(revisited.is_known_acyclic());
  REQUIRE_FALSE(revisited.has_porf_cache());

  const auto s2 = revisited.add_event(1, SendLabel{.destination = 2, .value = "y"});
  REQUIRE(revisited.is_known_acyclic());
  const auto r2 = revisited.add_event(2, make_receive_label_from_values<Value>({"y"}));
  REQUIRE(revisited.is_known_acyclic());
  revisited.set_reads_from(r2, s2);
  REQUIRE(revisited.is_known_acyclic());
}

TEST_CASE("exploration-graph checker re-arms known acyclicity for restricted graphs",
          "[model][consistency][exploration_graph]") {
  ExplorationGraph graph;
  const auto s1 = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r1 = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  const auto s2 = graph.add_event(2, SendLabel{.destination = 3, .value = "side"});
  graph.set_reads_from(r1, s1);

  auto restricted = graph.restrict({s1, r1, s2});
  REQUIRE_FALSE(restricted.is_known_acyclic());
  REQUIRE_FALSE(restricted.has_porf_cache());

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(restricted);

  REQUIRE(result.is_consistent());
  REQUIRE(restricted.is_known_acyclic());
  REQUIRE_FALSE(restricted.has_porf_cache());

  const auto s3 = restricted.add_event(1, SendLabel{.destination = 2, .value = "z"});
  REQUIRE(restricted.is_known_acyclic());
  const auto r2 = restricted.add_event(2, make_receive_label_from_values<Value>({"z"}));
  REQUIRE(restricted.is_known_acyclic());
  restricted.set_reads_from(r2, s3);
  REQUIRE(restricted.is_known_acyclic());
}

TEST_CASE(
    "exploration-graph checker overload still detects cycles after with_rf clears known acyclicity",
    "[model][consistency][exploration_graph]") {
  ExplorationGraph graph;
  const auto r1 = graph.add_event(1, make_receive_label<Value>());
  const auto s1 = graph.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto r2 = graph.add_event(2, make_receive_label<Value>());
  const auto s2 = graph.add_event(2, SendLabel{.destination = 1, .value = "b"});

  graph.set_reads_from(r2, s1);

  auto revisited = graph.with_rf(r1, s2);
  REQUIRE_FALSE(revisited.is_known_acyclic());
  REQUIRE_FALSE(revisited.has_porf_cache());

  const AsyncConsistencyChecker checker;
  const auto execution_result = checker.check(revisited.execution_graph());
  const auto exploration_result = checker.check(revisited);

  REQUIRE(issue_codes(exploration_result) == issue_codes(execution_result));
  REQUIRE(has_issue(exploration_result, ConsistencyIssueCode::CausalCycle));
  REQUIRE_FALSE(revisited.is_known_acyclic());
}

// --- Custom value type ---

TEST_CASE("consistency checker works with custom value type", "[model][consistency]") {
  struct Msg {
    int code{};
  };

  ExecutionGraphT<Msg> graph;
  const auto s = graph.add_event(1, SendLabelT<Msg>{.destination = 2, .value = Msg{.code = 42}});
  const auto r =
      graph.add_event(2, make_receive_label<Msg>([](const Msg& m) { return m.code == 42; }));
  graph.set_reads_from(r, s);

  const AsyncConsistencyCheckerT<Msg> checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("consistency checker with custom type detects value mismatch", "[model][consistency]") {
  struct Msg {
    int code{};
  };

  ExecutionGraphT<Msg> graph;
  const auto s = graph.add_event(1, SendLabelT<Msg>{.destination = 2, .value = Msg{.code = 99}});
  const auto r =
      graph.add_event(2, make_receive_label<Msg>([](const Msg& m) { return m.code == 42; }));
  graph.set_reads_from(r, s);

  const AsyncConsistencyCheckerT<Msg> checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, ConsistencyIssueCode::ReceiveValueMismatch));
}

// --- Larger graphs ---

TEST_CASE("four-thread diamond: no cycle when communication is acyclic", "[model][consistency]") {
  ExecutionGraph graph;

  // Thread 1 sends to threads 2 and 3
  const auto s1a = graph.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto s1b = graph.add_event(1, SendLabel{.destination = 3, .value = "b"});

  // Thread 2 receives from 1, sends to 4
  const auto r2 = graph.add_event(2, make_receive_label_from_values<Value>({"a"}));
  const auto s2 = graph.add_event(2, SendLabel{.destination = 4, .value = "c"});

  // Thread 3 receives from 1, sends to 4
  const auto r3 = graph.add_event(3, make_receive_label_from_values<Value>({"b"}));
  const auto s3 = graph.add_event(3, SendLabel{.destination = 4, .value = "d"});

  // Thread 4 receives from both 2 and 3
  const auto r4a = graph.add_event(4, make_receive_label_from_values<Value>({"c"}));
  const auto r4b = graph.add_event(4, make_receive_label_from_values<Value>({"d"}));

  graph.set_reads_from(r2, s1a);
  graph.set_reads_from(r3, s1b);
  graph.set_reads_from(r4a, s2);
  graph.set_reads_from(r4b, s3);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}

TEST_CASE("mixed events: sends, receives, blocks, errors in one graph", "[model][consistency]") {
  ExecutionGraph graph;

  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  static_cast<void>(graph.add_event(1, BlockLabel{}));
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"x"}));
  static_cast<void>(graph.add_event(2, ErrorLabel{}));
  static_cast<void>(graph.add_event(3, NondeterministicChoiceLabel{.value = "choice_a"}));

  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
}
