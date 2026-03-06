#include "dpor/model/consistency.hpp"
#include "dpor/model/execution_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace {
struct Payload {
  int id{};
};

struct Message {
  std::string kind{};
  int term{};
};

enum class ValidationResult {
  Ok,
  Invalid,
};

ValidationResult validate_message(const Message& message) {
  if (message.kind == "append_entries" && message.term >= 0) {
    return ValidationResult::Ok;
  }
  return ValidationResult::Invalid;
}

bool has_issue(
    const dpor::model::ConsistencyResult& result,
    const dpor::model::ConsistencyIssueCode code) {
  return std::any_of(
      result.issues.begin(),
      result.issues.end(),
      [code](const dpor::model::ConsistencyIssue& issue) {
        return issue.code == code;
      });
}
}  // namespace

TEST_CASE("event helper predicates classify labels", "[model][event]") {
  const dpor::model::Event receive{
      .thread = 1,
      .index = 0,
      .label = dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"v1", "v2"}),
  };

  const dpor::model::Event send{
      .thread = 2,
      .index = 0,
      .label = dpor::model::SendLabel{.destination = 1, .value = "v1"},
  };

  REQUIRE(dpor::model::is_receive(receive));
  REQUIRE_FALSE(dpor::model::is_send(receive));
  REQUIRE(dpor::model::is_send(send));
  REQUIRE_FALSE(dpor::model::is_receive(send));
}

TEST_CASE("execution graph tracks po and rf relations", "[model][graph]") {
  dpor::model::ExecutionGraph graph;

  const auto send_0_id = graph.add_event(1, dpor::model::SendLabel{.destination = 2, .value = "x"});
  const auto send_1_id = graph.add_event(1, dpor::model::SendLabel{.destination = 2, .value = "y"});
  const auto recv_id = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x", "y"}));

  graph.set_reads_from(recv_id, send_1_id);

  REQUIRE(graph.event(send_0_id).thread == 1);
  REQUIRE(graph.reads_from().size() == 1);
  REQUIRE(graph.send_event_ids().size() == 2);
  REQUIRE(graph.receive_event_ids().size() == 1);
  REQUIRE(graph.unread_send_event_ids() == std::vector<dpor::model::NodeId>{send_0_id});

  const auto po = graph.po_relation();
  const auto rf = graph.rf_relation();
  const auto po_then_rf = dpor::model::compose(po, rf);

  REQUIRE(po.contains(send_0_id, send_1_id));
  REQUIRE_FALSE(po.contains(send_1_id, send_0_id));
  REQUIRE(rf.contains(send_1_id, recv_id));
  REQUIRE(po_then_rf.contains(send_0_id, recv_id));
  REQUIRE(graph.event(send_0_id).index == 0);
  REQUIRE(graph.event(send_1_id).index == 1);
  REQUIRE(graph.event(recv_id).index == 0);
}

TEST_CASE("execution graph stores bottom reads-from without consuming sends", "[model][graph]") {
  dpor::model::ExecutionGraph graph;
  const auto send_id = graph.add_event(1, dpor::model::SendLabel{.destination = 2, .value = "x"});
  const auto recv_id = graph.add_event(
      2,
      dpor::model::make_nonblocking_receive_label<dpor::model::Value>());

  graph.set_reads_from_bottom(recv_id);

  REQUIRE(graph.reads_from().size() == 1);
  REQUIRE(graph.reads_from().at(recv_id).is_bottom());
  REQUIRE(graph.unread_send_event_ids() == std::vector<dpor::model::NodeId>{send_id});

  const auto rf = graph.rf_relation();
  REQUIRE_FALSE(rf.contains(send_id, recv_id));
}

TEST_CASE("async consistency checker accepts well-formed graph", "[model][consistency]") {
  dpor::model::ExecutionGraph graph;
  const auto send_id = graph.add_event(1, dpor::model::SendLabel{.destination = 2, .value = "ok"});
  const auto recv_id = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"ok"}));
  graph.set_reads_from(recv_id, send_id);

  const dpor::model::AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE(result.is_consistent());
  REQUIRE(result.issues.empty());
}

TEST_CASE("async consistency checker reports invalid reads-from endpoints", "[model][consistency]") {
  dpor::model::ExecutionGraph graph;
  const auto send_id = graph.add_event(1, dpor::model::SendLabel{.destination = 2, .value = "x"});
  const auto recv_id = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x"}));

  graph.set_reads_from(recv_id, 999);
  graph.set_reads_from(888, send_id);

  const dpor::model::AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, dpor::model::ConsistencyIssueCode::InvalidEventReference));
}

TEST_CASE("async consistency checker requires every receive to read exactly one send", "[model][consistency]") {
  dpor::model::ExecutionGraph graph;
  const auto recv_id = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x"}));

  const dpor::model::AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, dpor::model::ConsistencyIssueCode::MissingReadsFromForReceive));
  REQUIRE(graph.event(recv_id).thread == 2);
}

TEST_CASE("async consistency checker validates reads-from endpoint kinds", "[model][consistency]") {
  dpor::model::ExecutionGraph graph;
  const auto send_id = graph.add_event(1, dpor::model::SendLabel{.destination = 2, .value = "x"});
  const auto recv_0 = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x"}));
  const auto recv_1 = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x"}));

  graph.set_reads_from(send_id, send_id);
  graph.set_reads_from(recv_0, recv_1);
  graph.set_reads_from(recv_1, send_id);

  const dpor::model::AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, dpor::model::ConsistencyIssueCode::ReadsFromTargetNotReceive));
  REQUIRE(has_issue(result, dpor::model::ConsistencyIssueCode::ReadsFromSourceNotSend));
}

TEST_CASE("async consistency checker reports destination and value mismatches", "[model][consistency]") {
  dpor::model::ExecutionGraph graph;
  const auto send_bad_dst = graph.add_event(1, dpor::model::SendLabel{.destination = 7, .value = "x"});
  const auto recv_bad_dst = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x"}));
  const auto send_bad_value = graph.add_event(1, dpor::model::SendLabel{.destination = 3, .value = "y"});
  const auto recv_bad_value = graph.add_event(
      3,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x"}));

  graph.set_reads_from(recv_bad_dst, send_bad_dst);
  graph.set_reads_from(recv_bad_value, send_bad_value);

  const dpor::model::AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, dpor::model::ConsistencyIssueCode::ReceiveDestinationMismatch));
  REQUIRE(has_issue(result, dpor::model::ConsistencyIssueCode::ReceiveValueMismatch));
}

TEST_CASE("async consistency checker reports sends consumed by multiple receives", "[model][consistency]") {
  dpor::model::ExecutionGraph graph;
  const auto send_id = graph.add_event(1, dpor::model::SendLabel{.destination = 2, .value = "x"});
  const auto recv_0 = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x"}));
  const auto recv_1 = graph.add_event(
      2,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"x"}));

  graph.set_reads_from(recv_0, send_id);
  graph.set_reads_from(recv_1, send_id);

  const dpor::model::AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, dpor::model::ConsistencyIssueCode::SendConsumedMultipleTimes));
}

TEST_CASE("async consistency checker reports causal cycles", "[model][consistency]") {
  dpor::model::ExecutionGraph graph;

  const auto recv_t1 = graph.add_event_with_index(
      1,
      0,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"b"}));
  const auto send_t1 = graph.add_event_with_index(
      1,
      1,
      dpor::model::SendLabel{.destination = 2, .value = "a"});
  const auto recv_t2 = graph.add_event_with_index(
      2,
      0,
      dpor::model::make_receive_label_from_values<dpor::model::Value>(
          {"a"}));
  const auto send_t2 = graph.add_event_with_index(
      2,
      1,
      dpor::model::SendLabel{.destination = 1, .value = "b"});

  graph.set_reads_from(recv_t1, send_t2);
  graph.set_reads_from(recv_t2, send_t1);

  const dpor::model::AsyncConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(has_issue(result, dpor::model::ConsistencyIssueCode::CausalCycle));
}

TEST_CASE("execution graph supports custom value type", "[model][graph]") {
  dpor::model::ExecutionGraphT<Payload> graph;

  const auto send_id =
      graph.add_event(1, dpor::model::SendLabelT<Payload>{.destination = 2, .value = Payload{.id = 7}});
  const auto recv_id = graph.add_event(
      2,
      dpor::model::make_receive_label<Payload>(
          [](const Payload& payload) { return payload.id == 7; }));

  graph.set_reads_from(recv_id, send_id);

  REQUIRE(dpor::model::is_send(graph.event(send_id)));
  REQUIRE(dpor::model::is_receive(graph.event(recv_id)));
  REQUIRE(graph.unread_send_event_ids().empty());

  const auto* receive = dpor::model::as_receive(graph.event(recv_id));
  REQUIRE(receive != nullptr);
  REQUIRE(receive->accepts(Payload{.id = 7}));
  REQUIRE_FALSE(receive->accepts(Payload{.id = 8}));
}

TEST_CASE("custom message payload keeps typed fields", "[model][graph][consistency]") {
  dpor::model::ExecutionGraphT<Message> graph;

  const auto send_id = graph.add_event(
      3,
      dpor::model::SendLabelT<Message>{
          .destination = 4,
          .value = Message{.kind = "append_entries", .term = 9},
      });
  const auto recv_id = graph.add_event(
      4,
      dpor::model::make_receive_label<Message>(
          [](const Message& candidate) {
            return validate_message(candidate) == ValidationResult::Ok;
          }));

  graph.set_reads_from(recv_id, send_id);

  const auto* send = dpor::model::as_send(graph.event(send_id));
  REQUIRE(send != nullptr);
  REQUIRE(send->value.kind == "append_entries");
  REQUIRE(send->value.term == 9);

  const auto* receive = dpor::model::as_receive(graph.event(recv_id));
  REQUIRE(receive != nullptr);
  REQUIRE(receive->accepts(Message{.kind = "append_entries", .term = 4}));
  REQUIRE_FALSE(receive->accepts(Message{.kind = "request_vote", .term = 4}));

  const dpor::model::AsyncConsistencyCheckerT<Message> checker;
  const auto result = checker.check(graph);
  REQUIRE(result.is_consistent());
  REQUIRE(result.issues.empty());
}

TEST_CASE("execution graph supports explicit index insertion for replay", "[model][graph]") {
  dpor::model::ExecutionGraph graph;

  const auto e0 = graph.add_event_with_index(7, 3, dpor::model::SendLabel{.destination = 8, .value = "a"});
  const auto e1 = graph.add_event_with_index(7, 4, dpor::model::SendLabel{.destination = 8, .value = "b"});

  REQUIRE(graph.event(e0).index == 3);
  REQUIRE(graph.event(e1).index == 4);
  REQUIRE_THROWS_AS(
      graph.add_event_with_index(7, 4, dpor::model::SendLabel{.destination = 8, .value = "c"}),
      std::invalid_argument);
}
