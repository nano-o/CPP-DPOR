#include "dpor/model/consistency.hpp"
#include "dpor/model/execution_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
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
}  // namespace

TEST_CASE("event helper predicates classify labels", "[model][event]") {
  const dpor::model::Event receive{
      .thread = 1,
      .index = 0,
      .label = dpor::model::make_receive_label_from_values<dpor::model::Value>(
          dpor::model::ReceiveMode::Blocking,
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

  const auto send_0_id = graph.add_event(dpor::model::Event{
      .thread = 1,
      .index = 0,
      .label = dpor::model::SendLabel{.destination = 2, .value = "x"},
  });
  const auto send_1_id = graph.add_event(dpor::model::Event{
      .thread = 1,
      .index = 1,
      .label = dpor::model::SendLabel{.destination = 2, .value = "y"},
  });
  const auto recv_id = graph.add_event(dpor::model::Event{
      .thread = 2,
      .index = 0,
      .label = dpor::model::make_receive_label_from_values<dpor::model::Value>(
          dpor::model::ReceiveMode::Blocking,
          {"x", "y"}),
  });

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
}

TEST_CASE("p2p consistency checker is currently a stub", "[model][consistency]") {
  const dpor::model::ExecutionGraph graph;
  const dpor::model::P2PConsistencyChecker checker;
  const auto result = checker.check(graph);

  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(result.issues.size() == 1);
  REQUIRE(result.issues.front().code == dpor::model::ConsistencyIssueCode::UnimplementedCheck);
}

TEST_CASE("execution graph supports custom value type", "[model][graph]") {
  dpor::model::ExecutionGraphT<Payload> graph;

  const auto send_id = graph.add_event(dpor::model::EventT<Payload>{
      .thread = 1,
      .index = 0,
      .label = dpor::model::SendLabelT<Payload>{.destination = 2, .value = Payload{.id = 7}},
  });
  const auto recv_id = graph.add_event(dpor::model::EventT<Payload>{
      .thread = 2,
      .index = 0,
      .label = dpor::model::make_receive_label<Payload>(
          dpor::model::ReceiveMode::Blocking,
          [](const Payload& payload) { return payload.id == 7; }),
  });

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

  const auto send_id = graph.add_event(dpor::model::EventT<Message>{
      .thread = 3,
      .index = 1,
      .label = dpor::model::SendLabelT<Message>{
          .destination = 4,
          .value = Message{.kind = "append_entries", .term = 9},
      },
  });
  const auto recv_id = graph.add_event(dpor::model::EventT<Message>{
      .thread = 4,
      .index = 1,
      .label = dpor::model::make_receive_label<Message>(
          dpor::model::ReceiveMode::Blocking,
          [](const Message& candidate) {
            return validate_message(candidate) == ValidationResult::Ok;
          }),
  });

  graph.set_reads_from(recv_id, send_id);

  const auto* send = dpor::model::as_send(graph.event(send_id));
  REQUIRE(send != nullptr);
  REQUIRE(send->value.kind == "append_entries");
  REQUIRE(send->value.term == 9);

  const auto* receive = dpor::model::as_receive(graph.event(recv_id));
  REQUIRE(receive != nullptr);
  REQUIRE(receive->accepts(Message{.kind = "append_entries", .term = 4}));
  REQUIRE_FALSE(receive->accepts(Message{.kind = "request_vote", .term = 4}));

  const dpor::model::P2PConsistencyCheckerT<Message> checker;
  const auto result = checker.check(graph);
  REQUIRE_FALSE(result.is_consistent());
  REQUIRE(result.issues.front().code == dpor::model::ConsistencyIssueCode::UnimplementedCheck);
}
