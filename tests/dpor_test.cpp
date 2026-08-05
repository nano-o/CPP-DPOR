#include "dpor/algo/dpor.hpp"

#include "dpor/errors.hpp"
#include "dpor/model/consistency.hpp"

#include "support/oracle.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
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

// Branches only on nondeterministic choice. There are no sends, so there are no
// backward revisits and therefore no parallel spawn sites at all: this program
// checks that the scheduler stays correct when every worker but one is idle.
Program make_pure_nd_program() {
  Program program;

  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return NondeterministicChoiceLabel{.value = "a", .choices = {"a", "b", "c"}};
    }
    if (step == 1 && trace.size() == 1) {
      return NondeterministicChoiceLabel{.value = "p", .choices = {"p", "q"}};
    }
    return std::nullopt;
  };

  program.threads[2] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return NondeterministicChoiceLabel{.value = "u", .choices = {"u", "v", "w"}};
    }
    return std::nullopt;
  };

  return program;
}

// Branches only on reads-from choice: three sends contend for two blocking
// receives, so backward revisits -- the sole spawn site -- fire heavily.
Program make_pure_receive_program() {
  Program program;

  program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "m1"};
    }
    if (step == 1) {
      return SendLabel{.destination = 3, .value = "m2"};
    }
    return std::nullopt;
  };

  program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 3, .value = "m3"};
    }
    return std::nullopt;
  };

  program.threads[3] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step < 2 && trace.size() == step) {
      return make_receive_label_from_values<Value>({"m1", "m2", "m3"});
    }
    return std::nullopt;
  };

  return program;
}

// Nests all three branching frame kinds: a reads-from choice feeds a
// nondeterministic choice whose outcome selects the value of a later send,
// which in turn creates backward revisits against a downstream receive.
Program make_nested_mixed_program() {
  Program program;

  program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "req"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "dup"};
    }
    return std::nullopt;
  };

  program.threads[2] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"req", "dup"}, ReceiveMode::NonBlocking);
    }
    if (step == 1 && trace.size() == 1) {
      return NondeterministicChoiceLabel{.value = "ok", .choices = {"ok", "err"}};
    }
    if (step == 2 && trace.size() == 2) {
      return SendLabel{
          .destination = 3,
          .value = trace[0].is_bottom() ? "none" : trace[1].value(),
      };
    }
    return std::nullopt;
  };

  program.threads[3] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label_from_values<Value>({"ok", "err", "none"});
    }
    if (step == 1 && trace.size() == 1) {
      return NondeterministicChoiceLabel{.value = "keep", .choices = {"keep", "drop"}};
    }
    return std::nullopt;
  };

  return program;
}

// ND-only, but wide and deep enough that exploration certainly outlasts helper
// thread startup. The focused pure-ND program above is too small for that: the
// starvation split can only engage once peers are actually parked.
Program make_wide_nd_program() {
  Program program;
  for (ThreadId tid = 1; tid <= 2; ++tid) {
    program.threads[tid] = [](const ThreadTrace& trace,
                              std::size_t step) -> std::optional<EventLabel> {
      if (step < 6 && trace.size() == step) {
        return NondeterministicChoiceLabel{.value = "a", .choices = {"a", "b"}};
      }
      return std::nullopt;
    };
  }
  return program;
}

// Receive-only counterpart: four sends contend for four blocking receives, so
// receive frames carry several reads-from candidates each.
Program make_wide_receive_program() {
  Program program;
  for (ThreadId tid = 1; tid <= 4; ++tid) {
    program.threads[tid] = [tid](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
      if (step == 0) {
        return SendLabel{.destination = 5, .value = "m" + std::to_string(tid)};
      }
      return std::nullopt;
    };
  }
  program.threads[5] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step < 4 && trace.size() == step) {
      return make_receive_label_from_values<Value>({"m1", "m2", "m3", "m4"});
    }
    return std::nullopt;
  };
  return program;
}

// Deliberately larger than the host's logical CPU count so the wait/wake path
// is exercised with more sleepers than runnable work.
std::size_t oversubscribed_worker_count() {
  const auto hardware = std::thread::hardware_concurrency();
  const std::size_t base = hardware == 0 ? 4U : static_cast<std::size_t>(hardware);
  return std::min<std::size_t>(64U, std::max<std::size_t>(8U, base * 2U));
}

// Aggregate counts cannot catch a scheduler that drops one execution and
// duplicates another, so every parallel check compares exact signature sets
// against both sequential exploration and the oracle.
void require_parallel_matches_sequential_and_oracle(const Program& program,
                                                    const std::vector<std::size_t>& worker_counts,
                                                    const std::size_t max_queued_tasks) {
  const auto oracle = dpor::test_support::collect_oracle_stats(program);
  const auto sequential =
      collect_observed_executions(program, [](const DporConfig& config) { return verify(config); });

  REQUIRE(sequential.result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(sequential.unique == oracle.signatures);

  for (const auto workers : worker_counts) {
    CAPTURE(workers, max_queued_tasks);
    const auto parallel = collect_observed_executions(program, [&](const DporConfig& config) {
      ParallelVerifyOptions options;
      options.max_workers = workers;
      options.max_queued_tasks = max_queued_tasks;
      return verify_parallel(config, options);
    });

    REQUIRE(parallel.result.kind == VerifyResultKind::AllExecutionsExplored);
    REQUIRE(parallel.result.executions_explored == sequential.result.executions_explored);
    REQUIRE(parallel.unique == sequential.unique);
    REQUIRE(parallel.unique == oracle.signatures);
    // No execution explored twice.
    REQUIRE(parallel.unique.size() == parallel.observed.size());
  }
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

  REQUIRE_THROWS_AS(verify(config), dpor::precondition_error);
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

TEST_CASE("ND choice with duplicate options explores each distinct value once",
          "[algo][dpor][regression]") {
  DporConfig config;

  config.program.threads[1] = [](const ThreadTrace& trace,
                                 std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b", "a", "b", "a"},
      };
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 2);
  require_dpor_matches_oracle(config.program, "T1=[ND({a,b,a,b,a})]");
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
      program, ExplorationGraph{}, dpor::algo::detail::sorted_thread_ids(program), 0);
  REQUIRE(next.next.has_value());
  REQUIRE(next.next->first == 1);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<BlockLabel>(
      next.next->second));  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE_FALSE(next.suppressed_by_thread_event_limit);
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
      program, ExplorationGraph{}, dpor::algo::detail::sorted_thread_ids(program), 0);
  REQUIRE(next.next.has_value());
  REQUIRE(next.next->first == 1);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<ReceiveLabel>(
      next.next->second));                           // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::get<ReceiveLabel>(next.next->second)  // NOLINT(bugprone-unchecked-optional-access)
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

// --- max_thread_events ---

namespace {

// The behavior max_thread_events is claimed to be exactly equivalent to: a
// legal ThreadFunctionT that stops the thread once it has produced `bound`
// events. The engine's version skips the call instead of making it.
Program wrap_program_with_step_bound(const Program& program, const std::size_t bound) {
  Program wrapped;
  program.threads.for_each_assigned([&](const ThreadId tid, const ThreadFunction& thread_fn) {
    wrapped.threads[tid] = [thread_fn, bound](const ThreadTrace& trace,
                                              std::size_t step) -> std::optional<EventLabel> {
      if (step >= bound) {
        return std::nullopt;
      }
      return thread_fn(trace, step);
    };
  });
  return wrapped;
}

// A receiver that keeps working after its first receive, with two senders
// competing for that receive. The competition forces a backward revisit whose
// restriction deletes the receiver's later events, dropping it back below a
// small per-thread bound and re-enabling it. Both the receiver's first receive
// and T4's receive also start out unsatisfiable, so blocked-receive
// rescheduling runs too. Mirroring the simple max_depth tests would exercise
// neither path, which is exactly where the equivalence could fail.
Program make_revisit_and_block_program() {
  Program program;

  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    if (step == 1 && trace.size() == 1) {
      return SendLabel{.destination = 4, .value = "echo-" + trace[0].value()};
    }
    if (step == 2) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "a"};
    }
    return std::nullopt;
  };
  program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "b"};
    }
    return std::nullopt;
  };
  program.threads[4] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  return program;
}

// Thread 1 keeps sending forever, so it is always available to be capped.
Program make_unbounded_sender_program() {
  Program program;
  program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return SendLabel{.destination = 2, .value = "x"};
  };
  program.threads[2] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };
  return program;
}

ObservedRun run_bounded(const Program& program, const std::size_t max_thread_events,
                        const std::optional<std::size_t> workers) {
  ObservedRun run;
  std::mutex observed_mutex;

  DporConfig config;
  config.program = program;
  config.max_thread_events = max_thread_events;
  config.on_terminal_execution = [&](const ExplorationGraph& graph) {
    auto signature = graph_signature(graph);
    std::lock_guard lock(observed_mutex);
    run.observed.push_back(signature);
    run.unique.insert(std::move(signature));
  };

  if (workers.has_value()) {
    ParallelVerifyOptions options;
    options.max_workers = *workers;
    run.result = verify_parallel(config, options);
  } else {
    run.result = verify(config);
  }
  return run;
}

void require_bound_matches_wrapped_program(const Program& program, const std::size_t bound,
                                           const std::optional<std::size_t> workers) {
  CAPTURE(bound, workers.value_or(0));
  const auto bounded = run_bounded(program, bound, workers);
  const auto wrapped = run_bounded(wrap_program_with_step_bound(program, bound), 0, workers);

  REQUIRE(bounded.result.kind == VerifyResultKind::AllExplored);
  REQUIRE(wrapped.result.kind == VerifyResultKind::AllExplored);
  // The terminal *kinds* deliberately differ -- that difference is the whole
  // point of the bound -- but the explored graphs must not.
  REQUIRE(bounded.unique == wrapped.unique);
  REQUIRE(bounded.observed.size() == wrapped.observed.size());
  REQUIRE(bounded.result.executions_explored == wrapped.result.executions_explored);
  REQUIRE(wrapped.result.thread_event_limit_executions_explored == 0);
  REQUIRE(bounded.result.max_thread_event_depth_reached <= bound);
  REQUIRE(bounded.result.max_thread_event_depth_reached ==
          wrapped.result.max_thread_event_depth_reached);
}

}  // namespace

TEST_CASE("max_thread_events limits exploration", "[algo][dpor][thread_event_limit]") {
  DporConfig config;
  config.program = make_unbounded_sender_program();
  config.max_thread_events = 3;

  std::size_t observed_count = 0;
  bool saw_thread_event_limit_execution = false;
  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    ++observed_count;
    saw_thread_event_limit_execution = execution.is_thread_event_limit_execution();
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.full_executions_explored == 0);
  REQUIRE(result.blocked_executions_explored == 0);
  REQUIRE(result.error_executions_explored == 0);
  // The bound, not max_depth, is what stopped this.
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.thread_event_limit_executions_explored == 1);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(result.max_thread_event_depth_reached == 3);
  REQUIRE(observed_count == 1);
  REQUIRE(saw_thread_event_limit_execution);
}

TEST_CASE("max_thread_events caps each thread independently", "[algo][dpor][thread_event_limit]") {
  // A max_depth-style check at frame entry would kill the whole branch as soon
  // as the first thread reached the bound, leaving thread 2 with no events.
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return SendLabel{.destination = 3, .value = "from-1"};
  };
  config.program.threads[2] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return SendLabel{.destination = 3, .value = "from-2"};
  };
  config.program.threads[3] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };
  config.max_thread_events = 2;

  std::size_t observed_count = 0;
  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    ++observed_count;
    REQUIRE(execution.graph.thread_event_count(1) == 2);
    REQUIRE(execution.graph.thread_event_count(2) == 2);
  };

  const auto result = verify(config);
  REQUIRE(result.thread_event_limit_executions_explored == 1);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(result.max_thread_event_depth_reached == 2);
  REQUIRE(observed_count == 1);
}

TEST_CASE("a block synthesized as the cap-th event stays blocked",
          "[algo][dpor][thread_event_limit]") {
  // The case that rules out classifying on thread_event_count >= cap: thread 1
  // ends with thread_event_count == cap whose last event is an engine-injected
  // Block, which is a genuinely maximal blocked execution, not a truncated one.
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "x"};
    }
    if (step == 1) {
      return make_receive_label_from_values<Value>({"never-sent"});
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };
  config.max_thread_events = 2;

  const auto result = verify(config);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(result.blocked_executions_explored == 1);
  REQUIRE(result.thread_event_limit_executions_explored == 0);
  REQUIRE(result.max_thread_event_depth_reached == 2);
}

TEST_CASE("a block created at the cap is still rescheduled once a send appears",
          "[algo][dpor][thread_event_limit]") {
  // Removing the trailing Block drops the thread to cap - 1, so the engine
  // re-asks at a step still under the bound and the replacement receive fits.
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "go"};
    }
    if (step == 1) {
      return make_receive_label_from_values<Value>({"ack"});
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label_from_values<Value>({"go"});
    }
    if (step == 1 && trace.size() == 1) {
      return SendLabel{.destination = 1, .value = "ack"};
    }
    return std::nullopt;
  };
  config.max_thread_events = 2;

  std::size_t observed_count = 0;
  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    ++observed_count;
    // The Block was replaced by the real receive, so nothing is blocked.
    REQUIRE_FALSE(execution.graph.has_blocked_thread());
    REQUIRE(execution.graph.thread_event_count(1) == 2);
    REQUIRE(execution.graph.thread_event_count(2) == 2);
  };

  const auto result = verify(config);
  REQUIRE(observed_count == 1);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(result.blocked_executions_explored == 0);
  REQUIRE(result.max_thread_event_depth_reached == 2);
}

TEST_CASE("a program completing in exactly cap events is still thread-event-limit",
          "[algo][dpor][thread_event_limit]") {
  // Pinned deliberately. ThreadEventLimit is conservative: it means the engine
  // declined to *ask*, not that the bound truncated anything. Distinguishing
  // this case from a genuine truncation would require making the very call the
  // bound exists to avoid, so do not "fix" it into Full.
  auto make_config = [](const std::size_t max_thread_events) {
    DporConfig config;
    config.program.threads[1] = [](const ThreadTrace&,
                                   std::size_t step) -> std::optional<EventLabel> {
      if (step < 2) {
        return SendLabel{.destination = 2, .value = "x"};
      }
      return std::nullopt;
    };
    config.program.threads[2] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
      return std::nullopt;
    };
    config.max_thread_events = max_thread_events;
    return config;
  };

  const auto at_cap = verify(make_config(2));
  REQUIRE(at_cap.executions_explored == 1);
  REQUIRE(at_cap.thread_event_limit_executions_explored == 1);
  REQUIRE(at_cap.full_executions_explored == 0);

  // One event of headroom is all it takes to see the thread finish.
  const auto above_cap = verify(make_config(3));
  REQUIRE(above_cap.executions_explored == 1);
  REQUIRE(above_cap.thread_event_limit_executions_explored == 0);
  REQUIRE(above_cap.full_executions_explored == 1);
  REQUIRE(above_cap.max_thread_event_depth_reached == 2);
}

TEST_CASE("terminal-kind precedence is DepthLimit > Error > ThreadEventLimit > Blocked",
          "[algo][dpor][thread_event_limit]") {
  // Not a chosen ordering: max_depth is checked before compute_next_event and
  // Error is published immediately after it returns, so the suppression signal
  // is only ever consulted on the no-next-event path. Pinned so a refactor
  // cannot silently reorder it.
  SECTION("depth limit wins over the thread-event bound") {
    DporConfig config;
    config.program = make_unbounded_sender_program();
    config.max_depth = 1;
    config.max_thread_events = 1;

    const auto result = verify(config);
    REQUIRE(result.executions_explored == 1);
    REQUIRE(result.depth_limit_executions_explored == 1);
    REQUIRE(result.thread_event_limit_executions_explored == 0);
  }

  SECTION("an error wins over the thread-event bound") {
    DporConfig config;
    config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
      return SendLabel{.destination = 2, .value = "x"};
    };
    config.program.threads[2] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
      return ErrorLabel{};
    };
    config.max_thread_events = 1;

    const auto result = verify(config);
    REQUIRE(result.executions_explored == 1);
    REQUIRE(result.error_executions_explored == 1);
    REQUIRE(result.thread_event_limit_executions_explored == 0);
  }

  SECTION("the thread-event bound wins over a blocked thread") {
    DporConfig config;
    config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
      return SendLabel{.destination = 2, .value = "x"};
    };
    config.program.threads[2] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
      return make_receive_label_from_values<Value>({"never-sent"});
    };
    config.max_thread_events = 1;

    std::size_t observed_count = 0;
    config.on_terminal_execution = [&](const TerminalExecution& execution) {
      ++observed_count;
      // A thread really is blocked here; the kind still reports the bound,
      // because the execution is not known to be maximal.
      REQUIRE(execution.graph.has_blocked_thread());
      REQUIRE(execution.is_thread_event_limit_execution());
    };

    const auto result = verify(config);
    REQUIRE(observed_count == 1);
    REQUIRE(result.executions_explored == 1);
    REQUIRE(result.thread_event_limit_executions_explored == 1);
    REQUIRE(result.blocked_executions_explored == 0);
  }
}

TEST_CASE("max_thread_event_depth_reached reports the observed maximum when unbounded",
          "[algo][dpor][thread_event_limit]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step < 3) {
      return SendLabel{.destination = 2, .value = "x"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  const auto result = verify(config);
  REQUIRE(result.thread_event_limit_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.max_thread_event_depth_reached == 3);
}

TEST_CASE("bounded exploration matches an explicitly wrapped program",
          "[algo][dpor][thread_event_limit]") {
  // The load-bearing equivalence claim: the engine-side skip computes the same
  // next_P(G) as a thread function that returns nullopt past the bound.
  const auto revisit_program = make_revisit_and_block_program();
  const auto mixed_program = make_parallel_mixed_program();

  for (const std::size_t bound : {1U, 2U, 3U, 4U}) {
    require_bound_matches_wrapped_program(revisit_program, bound, std::nullopt);
    require_bound_matches_wrapped_program(mixed_program, bound, std::nullopt);
    require_bound_matches_wrapped_program(revisit_program, bound, std::optional<std::size_t>(4));
    require_bound_matches_wrapped_program(mixed_program, bound, std::optional<std::size_t>(8));
  }

  // The wrapped programs are ordinary programs, so the exhaustive oracle can
  // check the bounded exploration too.
  for (const std::size_t bound : {2U, 3U}) {
    require_dpor_matches_oracle(wrap_program_with_step_bound(revisit_program, bound),
                                "revisit/block program wrapped at step " + std::to_string(bound));
    require_dpor_matches_oracle(wrap_program_with_step_bound(mixed_program, bound),
                                "mixed program wrapped at step " + std::to_string(bound));
  }
}

TEST_CASE("verify_parallel reports thread-event-limit terminals",
          "[algo][dpor][parallel][thread_event_limit]") {
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
  program.threads[2] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };

  std::size_t observed_count = 0;
  std::size_t full_observed_count = 0;
  std::size_t thread_event_limit_observed_count = 0;
  std::mutex observed_mutex;

  DporConfig config;
  config.program = program;
  config.max_thread_events = 2;
  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    std::lock_guard lock(observed_mutex);
    ++observed_count;
    if (execution.is_full_execution()) {
      ++full_observed_count;
    }
    if (execution.is_thread_event_limit_execution()) {
      ++thread_event_limit_observed_count;
    }
  };

  ParallelVerifyOptions options;
  options.max_workers = 2;
  options.max_queued_tasks = 4;
  const auto result = verify_parallel(config, options);

  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.full_executions_explored == 1);
  REQUIRE(result.thread_event_limit_executions_explored == 1);
  REQUIRE(result.executions_explored == 2);
  REQUIRE(result.max_thread_event_depth_reached == 2);
  REQUIRE(observed_count == 2);
  REQUIRE(full_observed_count == 1);
  REQUIRE(thread_event_limit_observed_count == 1);
}

TEST_CASE("progress snapshots carry the thread-event-limit counters",
          "[algo][dpor][thread_event_limit]") {
  DporConfig config;
  config.program = make_unbounded_sender_program();
  config.max_thread_events = 4;
  config.progress_report_interval = std::chrono::milliseconds::zero();

  std::optional<ProgressSnapshot> final_snapshot;
  config.on_progress = [&](const ProgressSnapshot& snapshot) {
    if (snapshot.state != ProgressState::Running) {
      final_snapshot = snapshot;
    }
  };

  const auto result = verify(config);
  REQUIRE(final_snapshot.has_value());
  REQUIRE(final_snapshot->thread_event_limit_executions ==  // NOLINT
          result.thread_event_limit_executions_explored);
  REQUIRE(final_snapshot->max_thread_event_depth ==  // NOLINT
          result.max_thread_event_depth_reached);
  REQUIRE(final_snapshot->terminal_executions == result.executions_explored);  // NOLINT
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

  REQUIRE_THROWS_AS(verify(config), dpor::user_code_error);
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
      program, ExplorationGraph{}, dpor::algo::detail::sorted_thread_ids(program), 0);
  REQUIRE(first.next.has_value());
  REQUIRE(first.next->first == 1);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<BlockLabel>(
      first.next->second));  // NOLINT(bugprone-unchecked-optional-access)

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
      program, partial, dpor::algo::detail::sorted_thread_ids(program), 0);
  REQUIRE(first.next.has_value());
  REQUIRE(first.next->first == 1);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<BlockLabel>(
      first.next->second));  // NOLINT(bugprone-unchecked-optional-access)
  static_cast<void>(partial.add_event(
      first.next->first, first.next->second));  // NOLINT(bugprone-unchecked-optional-access)

  const auto second = dpor::algo::detail::compute_next_event(
      program, partial, dpor::algo::detail::sorted_thread_ids(program), 0);
  REQUIRE(second.next.has_value());
  REQUIRE(second.next->first == 2);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(std::holds_alternative<BlockLabel>(
      second.next->second));  // NOLINT(bugprone-unchecked-optional-access)

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

TEST_CASE("blocking receive with no sender publishes a blocked terminal execution",
          "[algo][dpor][blocked]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  std::vector<TerminalExecutionKind> kinds;
  bool observer_saw_block_event = false;
  bool observer_blocked_flag = false;
  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    kinds.push_back(execution.kind);
    observer_blocked_flag = execution.is_blocked_execution();
    for (const auto& evt : execution.graph.events()) {
      if (is_block(evt)) {
        observer_saw_block_event = true;
      }
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(result.blocked_executions_explored == 1);
  REQUIRE(result.full_executions_explored == 0);
  REQUIRE(kinds == std::vector<TerminalExecutionKind>{TerminalExecutionKind::Blocked});
  REQUIRE(observer_blocked_flag);
  REQUIRE(observer_saw_block_event);
  require_dpor_matches_oracle(config.program, "T1=[Rb(*)] with no sender");
}

TEST_CASE("ND-gated sender yields one full and one blocked terminal execution",
          "[algo][dpor][blocked]") {
  DporConfig config;

  // T1 blocks on a receive; T2 nondeterministically decides whether to send,
  // so exactly one branch satisfies the receive and one leaves it blocked.
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace& trace,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "send",
          .choices = {"send", "skip"},
      };
    }
    if (step == 1 && !trace.empty() && trace[0].value() == "send") {
      return SendLabel{.destination = 1, .value = "m"};
    }
    return std::nullopt;
  };

  const auto sequential = verify(config);
  REQUIRE(sequential.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(sequential.executions_explored == 2);
  REQUIRE(sequential.full_executions_explored == 1);
  REQUIRE(sequential.blocked_executions_explored == 1);
  require_dpor_matches_oracle(config.program, "T1=[Rb(*)]; T2=[ND({send,skip}), send -> S(1,m)]");

  ParallelVerifyOptions options;
  options.max_workers = 2;
  options.max_queued_tasks = 4;
  const auto parallel = verify_parallel(config, options);
  REQUIRE(parallel.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(parallel.executions_explored == sequential.executions_explored);
  REQUIRE(parallel.full_executions_explored == sequential.full_executions_explored);
  REQUIRE(parallel.blocked_executions_explored == sequential.blocked_executions_explored);
}

TEST_CASE("depth-limited branch keeps DepthLimit kind even when a thread is blocked",
          "[algo][dpor][blocked]") {
  DporConfig config;
  config.max_depth = 1;
  // T1's receive matches nothing, so it blocks immediately; the depth limit
  // then fires on a graph that contains a Block event.
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>([](const Value&) { return false; });
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "unmatched"};
    }
    return std::nullopt;
  };

  bool depth_limit_graph_has_block = false;
  config.on_terminal_execution = [&](const TerminalExecution& execution) {
    REQUIRE(execution.kind == TerminalExecutionKind::DepthLimit);
    for (const auto& evt : execution.graph.events()) {
      if (is_block(evt)) {
        depth_limit_graph_has_block = true;
      }
    }
  };

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.depth_limit_executions_explored == result.executions_explored);
  REQUIRE(result.blocked_executions_explored == 0);
  REQUIRE(result.full_executions_explored == 0);
  REQUIRE(depth_limit_graph_has_block);
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
  REQUIRE(dpor::algo::detail::get_cons_tiebreaker(graph, r0, CommunicationModel::FifoP2P) == s20);
}

TEST_CASE("fifo rf rewrite helper accepts safe rewires on known-acyclic graphs",
          "[algo][dpor][fifo_p2p][regression]") {
  ExplorationGraph graph;

  const auto s10 = graph.add_event(1, SendLabel{.destination = 3, .value = "a"});
  const auto s20 = graph.add_event(2, SendLabel{.destination = 3, .value = "a"});
  const auto r0 = graph.add_event(3, make_receive_label_from_values<Value>({"a"}));

  graph.set_reads_from(r0, s10);
  REQUIRE(graph.is_known_acyclic());

  REQUIRE(
      dpor::algo::detail::rf_rewrite_is_consistent(graph, r0, s20, CommunicationModel::FifoP2P));
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
  REQUIRE(dpor::algo::detail::get_cons_tiebreaker_masked(graph, previous, receive) == send_current);

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

namespace {

// Paper example 2.5 (NNR), naive timeout encoding:
//   recv_timeout() ≜ if (nondet({wait,timeout})) { recv() } else { ⊥ }
// with no sender, so every thread that chooses to wait blocks forever.
[[nodiscard]] Program make_naive_nnr_program(const std::size_t thread_count) {
  Program program;
  for (ThreadId tid = 1; tid <= thread_count; ++tid) {
    program.threads[tid] = [](const ThreadTrace& trace,
                              std::size_t step) -> std::optional<EventLabel> {
      if (step == 0) {
        return NondeterministicChoiceLabel{
            .value = "wait",
            .choices = {"wait", "timeout"},
        };
      }
      if (step == 1 && !trace.empty() && trace[0].value() == "wait") {
        return make_receive_label<Value>();
      }
      return std::nullopt;
    };
  }
  return program;
}

}  // namespace

TEST_CASE("paper ex 2.5: naive NNR timeout encoding yields 2^N executions, all but one blocked",
          "[algo][dpor][paper][blocked]") {
  constexpr std::size_t kThreadCount = 3;
  DporConfig config;
  config.program = make_naive_nnr_program(kThreadCount);

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  // One terminal per nondet combination; no message is ever sent, so every
  // combination with at least one waiting thread ends blocked.
  REQUIRE(result.executions_explored == 8);
  REQUIRE(result.blocked_executions_explored == 7);
  REQUIRE(result.full_executions_explored == 1);
  require_dpor_matches_oracle(config.program,
                              "NNR naive: Ti=[ND({wait,timeout}), wait -> Rb(*)] x3");
}

TEST_CASE("paper ex 2.5: non-blocking NNR encoding collapses to one full execution",
          "[algo][dpor][paper][blocked]") {
  constexpr std::size_t kThreadCount = 3;
  DporConfig config;
  for (ThreadId tid = 1; tid <= kThreadCount; ++tid) {
    config.program.threads[tid] = [](const ThreadTrace&,
                                     std::size_t step) -> std::optional<EventLabel> {
      if (step == 0) {
        return make_nonblocking_receive_label<Value>();
      }
      return std::nullopt;
    };
  }

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored == 1);
  REQUIRE(result.full_executions_explored == 1);
  REQUIRE(result.blocked_executions_explored == 0);
  require_dpor_matches_oracle(config.program, "NNR nb: Ti=[Rnb(*)] x3");
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
  require_dpor_matches_oracle(config.program, "fifo_p2p: T1=[S(2,a),S(2,b)]; T2=[Rb(*)]",
                              CommunicationModel::FifoP2P);
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
  require_dpor_matches_oracle(config.program, "fifo_p2p: T1=[S(2,1),S(2,2)]; T2=[Rb({2}),Rb({1})]",
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
  require_dpor_matches_oracle(config.program,
                              "fifo_p2p: T1=[S(3,a)]; T2=[S(3,b)]; T3=[Rb({b}),Rb({a})]",
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

  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
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
  program.threads[2] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };
  program.threads[3] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
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
      program,
      "fifo_p2p revisit prune: T1=[S(2,a),Rb({token}),S(2,b)]; T2=[Rb(*)]; T3=[S(1,token)]",
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

TEST_CASE("verify_parallel matches sequential and oracle execution sets on pure ND branching",
          "[algo][dpor][parallel]") {
  require_parallel_matches_sequential_and_oracle(make_pure_nd_program(),
                                                 {1, 2, 4, oversubscribed_worker_count()}, 16);
}

TEST_CASE("verify_parallel matches sequential and oracle execution sets on pure receive branching",
          "[algo][dpor][parallel]") {
  require_parallel_matches_sequential_and_oracle(make_pure_receive_program(),
                                                 {1, 2, 4, oversubscribed_worker_count()}, 16);
}

TEST_CASE("verify_parallel matches sequential and oracle execution sets on nested mixed branching",
          "[algo][dpor][parallel]") {
  const auto program = make_nested_mixed_program();
  const std::vector<std::size_t> worker_counts{1, 2, 4, oversubscribed_worker_count()};

  SECTION("default queue budget") {
    require_parallel_matches_sequential_and_oracle(program, worker_counts, 16);
  }

  // A one-slot queue forces most handoffs to fail and fall back to local
  // exploration, so it exercises the ownership-restore path under contention.
  SECTION("tiny queue budget") {
    require_parallel_matches_sequential_and_oracle(program, worker_counts, 1);
  }
}

TEST_CASE("verify_parallel matches sequential across worker counts on mixed branching",
          "[algo][dpor][parallel]") {
  require_parallel_matches_sequential_and_oracle(make_parallel_mixed_program(),
                                                 {1, 2, 4, oversubscribed_worker_count()}, 16);
}

// Quiescence is signalled by exactly one broadcast, sent by whichever worker
// happens to drain the last task. If that wake is ever missed, a worker stays
// parked in queue_cv_.wait() and verify_parallel() hangs in join() rather than
// failing an assertion -- so this test guards a hang, and it repeats to make an
// intermittent miss likely to show up.
TEST_CASE("verify_parallel reaches quiescence repeatedly without stranding workers",
          "[algo][dpor][parallel]") {
  const auto program = make_parallel_mixed_program();
  const auto sequential =
      collect_observed_executions(program, [](const DporConfig& config) { return verify(config); });
  REQUIRE(sequential.result.kind == VerifyResultKind::AllExecutionsExplored);

  constexpr std::size_t kRepeats = 50;
  for (std::size_t iteration = 0; iteration < kRepeats; ++iteration) {
    CAPTURE(iteration);
    const auto parallel = collect_observed_executions(program, [](const DporConfig& config) {
      ParallelVerifyOptions options;
      // More workers than the program can keep busy, and a queue too small to
      // hold the backlog: most workers spend the run asleep and must still be
      // woken exactly once at completion.
      options.max_workers = 8;
      options.max_queued_tasks = 1;
      return verify_parallel(config, options);
    });

    REQUIRE(parallel.result.kind == VerifyResultKind::AllExecutionsExplored);
    REQUIRE(parallel.unique == sequential.unique);
    REQUIRE(parallel.result.executions_explored == sequential.result.executions_explored);
  }
}

namespace {
// The starvation split is opportunistic by construction: it only fires while a
// peer is parked, so a single run can legitimately explore the whole tree
// without ever offering an alternative. Correctness is asserted on every
// attempt; activation only has to be observed once.
struct SplitActivation {
  std::size_t nd_splits{0};
  std::size_t receive_splits{0};
};

SplitActivation run_until_split_observed(const Program& program, const bool want_nd_splits) {
  const auto sequential =
      collect_observed_executions(program, [](const DporConfig& config) { return verify(config); });
  REQUIRE(sequential.result.kind == VerifyResultKind::AllExecutionsExplored);

  SplitActivation seen;
  for (int attempt = 0; attempt < 10; ++attempt) {
    CAPTURE(attempt);
    VerifyResult parallel_result{};
    const auto parallel = collect_observed_executions(program, [&](const DporConfig& config) {
      ParallelVerifyOptions options;
      options.max_workers = 8;
      options.max_queued_tasks = 32;
      // Check the idle count on every branch: the batched default can skip
      // right past a small program's entire branching surface.
      options.split_poll_interval_steps = 1;
      parallel_result = verify_parallel(config, options);
      return parallel_result;
    });

    REQUIRE(parallel.result.kind == VerifyResultKind::AllExecutionsExplored);
    REQUIRE(parallel.result.executions_explored == sequential.result.executions_explored);
    REQUIRE(parallel.unique == sequential.unique);
    REQUIRE(parallel.unique.size() == parallel.observed.size());

    seen.nd_splits += parallel_result.nd_splits;
    seen.receive_splits += parallel_result.receive_splits;
    if (want_nd_splits ? seen.nd_splits > 0 : seen.receive_splits > 0) {
      break;
    }
  }
  return seen;
}
}  // namespace

TEST_CASE("verify_parallel splits ND alternatives to idle workers", "[algo][dpor][parallel]") {
  const auto seen = run_until_split_observed(make_wide_nd_program(), true);
  // Assert on the counter, not on wall-clock: this is what proves the new
  // spawn site actually engaged rather than merely not breaking anything.
  REQUIRE(seen.nd_splits > 0);
}

TEST_CASE("verify_parallel splits receive alternatives to idle workers", "[algo][dpor][parallel]") {
  const auto seen = run_until_split_observed(make_wide_receive_program(), false);
  REQUIRE(seen.receive_splits > 0);
}

TEST_CASE("verify_parallel performs no splits with a single worker", "[algo][dpor][parallel]") {
  const auto program = make_wide_nd_program();
  const auto parallel = collect_observed_executions(program, [](const DporConfig& config) {
    ParallelVerifyOptions options;
    options.max_workers = 1;
    options.split_poll_interval_steps = 1;
    const auto result = verify_parallel(config, options);
    REQUIRE(result.nd_splits == 0);
    REQUIRE(result.receive_splits == 0);
    return result;
  });
  REQUIRE(parallel.result.kind == VerifyResultKind::AllExecutionsExplored);
}

TEST_CASE("verify reports no parallel splits in sequential mode", "[algo][dpor]") {
  DporConfig config;
  config.program = make_wide_nd_program();
  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.nd_splits == 0);
  REQUIRE(result.receive_splits == 0);
}

TEST_CASE("verify_parallel matches sequential and oracle with splitting forced on",
          "[algo][dpor][parallel]") {
  // split_poll_interval_steps = 1 maximises how often the split path is taken,
  // so these run the ownership-transfer path far harder than a default run.
  const std::vector<std::size_t> worker_counts{2, 4, oversubscribed_worker_count()};
  for (const auto workers : worker_counts) {
    CAPTURE(workers);
    for (const auto& program : {make_pure_nd_program(), make_pure_receive_program(),
                                make_nested_mixed_program(), make_parallel_mixed_program()}) {
      const auto oracle = dpor::test_support::collect_oracle_stats(program);
      const auto sequential = collect_observed_executions(
          program, [](const DporConfig& config) { return verify(config); });
      const auto parallel = collect_observed_executions(program, [&](const DporConfig& config) {
        ParallelVerifyOptions options;
        options.max_workers = workers;
        options.max_queued_tasks = 4;
        options.split_poll_interval_steps = 1;
        return verify_parallel(config, options);
      });

      REQUIRE(parallel.result.kind == VerifyResultKind::AllExecutionsExplored);
      REQUIRE(parallel.result.executions_explored == sequential.result.executions_explored);
      REQUIRE(parallel.unique == sequential.unique);
      REQUIRE(parallel.unique == oracle.signatures);
      REQUIRE(parallel.unique.size() == parallel.observed.size());
    }
  }
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

TEST_CASE("verify_parallel rejects reentrant invocation from a worker callback",
          "[algo][dpor][parallel]") {
  Program inner_program;
  inner_program.threads[1] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "inner"};
    }
    return std::nullopt;
  };

  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 1, .value = "outer"};
    }
    return std::nullopt;
  };
  config.on_terminal_execution = [inner_program](const ExplorationGraph&) {
    DporConfig inner_config;
    inner_config.program = inner_program;
    ParallelVerifyOptions inner_options;
    inner_options.max_workers = 1;
    static_cast<void>(verify_parallel(inner_config, inner_options));
  };

  ParallelVerifyOptions options;
  options.max_workers = 2;
  options.max_queued_tasks = 4;

  // The reentrancy precondition_error is raised while inside the user's
  // terminal observer, so it crosses the callback boundary tagged as
  // user_code_error with the precondition_error preserved as the original.
  try {
    static_cast<void>(verify_parallel(config, options));
    FAIL("expected verify_parallel to throw");
  } catch (const dpor::user_code_error& err) {
    REQUIRE(err.kind() == dpor::UserCallbackKind::TerminalObserver);
    REQUIRE(err.has_original());
    REQUIRE_THROWS_AS(err.rethrow_original(), dpor::precondition_error);
  }
}

TEST_CASE("verify and verify_parallel reject configs that set both terminal observers",
          "[algo][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };
  config.on_terminal_execution = [](const ExplorationGraph&) {};
  config.on_execution = [](const ExplorationGraph&) {};

  REQUIRE_THROWS_AS(verify(config), dpor::precondition_error);

  ParallelVerifyOptions options;
  options.max_workers = 1;
  REQUIRE_THROWS_AS(verify_parallel(config, options), dpor::precondition_error);
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

TEST_CASE("verify handles deep ND execution without recursive stack growth",
          "[algo][dpor][stack]") {
  constexpr std::size_t kDepth = 12000;

  Program program;
  program.threads[1] = [](const ThreadTrace& trace,
                          const std::size_t step) -> std::optional<EventLabel> {
    if (trace.size() != step) {
      throw std::logic_error("ND stack regression: thread_trace must track prior ND choices");
    }
    if (step < kDepth) {
      return NondeterministicChoiceLabel{
          .value = "tick",
          .choices = {"tick"},
      };
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

TEST_CASE("verify handles deep receive execution without recursive stack growth",
          "[algo][dpor][stack]") {
  constexpr std::size_t kReceivePairs = 6000;

  Program program;
  program.threads[1] = [](const ThreadTrace& trace,
                          const std::size_t step) -> std::optional<EventLabel> {
    if (trace.size() != step / 2U) {
      throw std::logic_error(
          "receive stack regression: receive observations must remain in thread_trace");
    }
    if (step >= kReceivePairs * 2U) {
      return std::nullopt;
    }
    if ((step % 2U) == 0U) {
      return SendLabel{.destination = 1, .value = "tick"};
    }
    return make_receive_label_from_values<Value>({"tick"});
  };

  DporConfig config;
  config.program = program;
  config.max_depth = (kReceivePairs * 2U) + 1U;

  const auto result = verify(config);
  REQUIRE(result.kind == VerifyResultKind::AllExplored);
  REQUIRE(result.full_executions_explored == 1);
  REQUIRE(result.error_executions_explored == 0);
  REQUIRE(result.depth_limit_executions_explored == 0);
  REQUIRE(result.executions_explored == 1);
}

TEST_CASE(
    "verify_parallel with one worker handles a deep linear execution without recursive stack "
    "growth",
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
