#include "dpor/algo/dpor.hpp"
#include "dpor/model/consistency.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
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

struct OracleTransition {
  ThreadId thread{};
  EventLabel label{};
  std::optional<ExplorationGraph::EventId> rf_source{};
};

std::vector<OracleTransition> enumerate_enabled_transitions(
    const Program& program,
    const ExplorationGraph& graph) {
  std::vector<ThreadId> thread_ids;
  thread_ids.reserve(program.threads.size());
  for (const auto& [tid, _] : program.threads) {
    thread_ids.push_back(tid);
  }
  std::sort(thread_ids.begin(), thread_ids.end());

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

    if (const auto* recv = std::get_if<ReceiveLabel>(&*next_label)) {
      for (const auto send_id : graph.unread_send_event_ids()) {
        const auto* send = as_send(graph.event(send_id));
        if (send != nullptr &&
            send->destination == tid &&
            recv->accepts(send->value)) {
          transitions.push_back(OracleTransition{
              .thread = tid,
              .label = *next_label,
              .rf_source = send_id,
          });
        }
      }
      continue;
    }

    if (const auto* nd = std::get_if<NondeterministicChoiceLabel>(&*next_label)) {
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
            .label = EventLabel{label},
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

void enumerate_consistent_executions(
    const Program& program,
    const ExplorationGraph& graph,
    AsyncConsistencyChecker& checker,
    std::set<std::string>& signatures) {
  const auto transitions = enumerate_enabled_transitions(program, graph);
  if (transitions.empty()) {
    signatures.insert(graph_signature(graph));
    return;
  }

  for (const auto& transition : transitions) {
    auto next_graph = graph;
    const auto event_id = next_graph.add_event(transition.thread, transition.label);
    if (transition.rf_source.has_value()) {
      next_graph.set_reads_from(event_id, *transition.rf_source);
    }

    // Soundness oracle follows Must's "visit-if-consistent" rule.
    const auto consistency = checker.check(next_graph.execution_graph());
    if (!consistency.is_consistent()) {
      continue;
    }

    enumerate_consistent_executions(program, next_graph, checker, signatures);
  }
}

std::set<std::string> collect_oracle_signatures(const Program& program) {
  AsyncConsistencyChecker checker;
  std::set<std::string> signatures;
  enumerate_consistent_executions(program, ExplorationGraph{}, checker, signatures);
  return signatures;
}

enum class ScriptOpKind {
  SendFixed,
  SendFromLastTrace,
  ReceiveAny,
  ReceiveValues,
  ReceiveLastTraceValue,
  NondeterministicChoice,
  Block,
};

struct ScriptOp {
  ScriptOpKind kind{ScriptOpKind::SendFixed};
  ThreadId destination{0};
  Value value{};
  std::vector<Value> values{};
};

using ThreadScript = std::vector<ScriptOp>;
using ProgramSpec = std::map<ThreadId, ThreadScript>;

Program build_program_from_spec(const ProgramSpec& spec) {
  Program program;

  for (const auto& [tid, script] : spec) {
    program.threads[tid] = [script](const ThreadTrace& trace, std::size_t step)
        -> std::optional<EventLabel> {
      if (step >= script.size()) {
        return std::nullopt;
      }

      const auto& op = script[step];
      switch (op.kind) {
        case ScriptOpKind::SendFixed:
          return SendLabel{
              .destination = op.destination,
              .value = op.value,
          };
        case ScriptOpKind::SendFromLastTrace:
          if (trace.empty()) {
            return std::nullopt;
          }
          return SendLabel{
              .destination = op.destination,
              .value = trace.back(),
          };
        case ScriptOpKind::ReceiveAny:
          return make_receive_label<Value>();
        case ScriptOpKind::ReceiveValues:
          return make_receive_label_from_values<Value>(op.values);
        case ScriptOpKind::ReceiveLastTraceValue:
          if (trace.empty()) {
            return std::nullopt;
          }
          return make_receive_label<Value>(
              [expected = trace.back()](const Value& candidate) {
                return candidate == expected;
              });
        case ScriptOpKind::NondeterministicChoice:
          if (!op.values.empty()) {
            return NondeterministicChoiceLabel{
                .value = op.values.front(),
                .choices = op.values,
            };
          }
          return NondeterministicChoiceLabel{
              .value = op.value,
              .choices = {},
          };
        case ScriptOpKind::Block:
          return BlockLabel{};
      }
      return std::nullopt;
    };
  }

  return program;
}

std::string script_op_to_string(const ScriptOp& op) {
  std::ostringstream oss;
  switch (op.kind) {
    case ScriptOpKind::SendFixed:
      oss << "S(" << op.destination << "," << op.value << ")";
      break;
    case ScriptOpKind::SendFromLastTrace:
      oss << "S(" << op.destination << ",trace[-1])";
      break;
    case ScriptOpKind::ReceiveAny:
      oss << "R(*)";
      break;
    case ScriptOpKind::ReceiveValues:
      oss << "R({";
      for (std::size_t i = 0; i < op.values.size(); ++i) {
        if (i != 0) {
          oss << ",";
        }
        oss << op.values[i];
      }
      oss << "})";
      break;
    case ScriptOpKind::ReceiveLastTraceValue:
      oss << "R(trace[-1])";
      break;
    case ScriptOpKind::NondeterministicChoice:
      oss << "ND({";
      for (std::size_t i = 0; i < op.values.size(); ++i) {
        if (i != 0) {
          oss << ",";
        }
        oss << op.values[i];
      }
      oss << "})";
      break;
    case ScriptOpKind::Block:
      oss << "B";
      break;
  }
  return oss.str();
}

std::string spec_to_string(const ProgramSpec& spec) {
  std::ostringstream oss;
  bool first_thread = true;
  for (const auto& [tid, script] : spec) {
    if (!first_thread) {
      oss << " ; ";
    }
    first_thread = false;
    oss << "T" << tid << "=[";
    for (std::size_t i = 0; i < script.size(); ++i) {
      if (i != 0) {
        oss << ", ";
      }
      oss << script_op_to_string(script[i]);
    }
    oss << "]";
  }
  return oss.str();
}

std::vector<ProgramSpec> generate_stress_specs(
    std::size_t target_count, std::uint32_t seed = 0xC0FFEEu) {
  std::vector<ProgramSpec> specs;
  std::set<std::string> seen_specs;

  constexpr std::array<const char*, 3> kValues = {"a", "b", "c"};
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> receiver_dist(1, 3);
  std::uniform_int_distribution<int> value_dist(0, 2);
  std::uniform_int_distribution<int> receiver_pattern_dist(0, 7);
  std::bernoulli_distribution coin_flip(0.5);
  std::bernoulli_distribution one_in_three(1.0 / 3.0);

  std::size_t attempts = 0;
  while (specs.size() < target_count && attempts < target_count * 80U) {
    ++attempts;

    const auto receiver = static_cast<ThreadId>(receiver_dist(rng));
    const auto sender1 = static_cast<ThreadId>((receiver % 3U) + 1U);
    const auto sender2 = static_cast<ThreadId>((sender1 % 3U) + 1U);

    const auto i1 = value_dist(rng);
    const auto i2 = value_dist(rng);
    const Value v1 = kValues[i1];
    const Value v2 = kValues[i2];
    const Value v1_alt = kValues[(i1 + 1) % 3];
    const Value v2_alt = kValues[(i2 + 1) % 3];

    ProgramSpec spec;

    auto& sender1_script = spec[sender1];
    if (coin_flip(rng)) {
      sender1_script.push_back(ScriptOp{
          .kind = ScriptOpKind::NondeterministicChoice,
          .values = {v1, v1_alt},
      });
      sender1_script.push_back(ScriptOp{
          .kind = ScriptOpKind::SendFromLastTrace,
          .destination = receiver,
      });
    } else {
      sender1_script.push_back(ScriptOp{
          .kind = ScriptOpKind::SendFixed,
          .destination = receiver,
          .value = v1,
      });
    }
    if (one_in_three(rng)) {
      sender1_script.push_back(ScriptOp{
          .kind = ScriptOpKind::SendFixed,
          .destination = receiver,
          .value = v1_alt,
      });
    }

    auto& sender2_script = spec[sender2];
    sender2_script.push_back(ScriptOp{
        .kind = ScriptOpKind::SendFixed,
        .destination = receiver,
        .value = v2,
    });
    if (coin_flip(rng)) {
      sender2_script.push_back(ScriptOp{
          .kind = ScriptOpKind::SendFixed,
          .destination = receiver,
          .value = v2_alt,
      });
    }

    auto& receiver_script = spec[receiver];
    switch (receiver_pattern_dist(rng)) {
      case 0:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveAny,
        });
        break;
      case 1:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v1},
        });
        break;
      case 2:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v2},
        });
        break;
      case 3:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveAny,
        });
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveAny,
        });
        break;
      case 4:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v1},
        });
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v2},
        });
        break;
      case 5:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v2},
        });
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v1},
        });
        break;
      case 6:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v1, v2},
        });
        break;
      case 7:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveAny,
        });
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveLastTraceValue,
        });
        break;
      default:
        break;
    }

    if (one_in_three(rng)) {
      const auto block_thread = static_cast<ThreadId>(receiver_dist(rng));
      spec[block_thread].push_back(ScriptOp{
          .kind = ScriptOpKind::Block,
      });
    }

    std::size_t send_count = 0;
    std::size_t receive_count = 0;
    std::size_t nd_count = 0;
    std::size_t total_ops = 0;
    for (const auto& [_, script] : spec) {
      total_ops += script.size();
      for (const auto& op : script) {
        switch (op.kind) {
          case ScriptOpKind::SendFixed:
          case ScriptOpKind::SendFromLastTrace:
            ++send_count;
            break;
          case ScriptOpKind::ReceiveAny:
          case ScriptOpKind::ReceiveValues:
          case ScriptOpKind::ReceiveLastTraceValue:
            ++receive_count;
            break;
          case ScriptOpKind::NondeterministicChoice:
            ++nd_count;
            break;
          case ScriptOpKind::Block:
            break;
        }
      }
    }

    // Keep programs small-but-nontrivial so exhaustive oracle stays cheap.
    if (send_count < 2 || receive_count == 0 || total_ops > 8 || nd_count > 1) {
      continue;
    }

    const auto key = spec_to_string(spec);
    if (seen_specs.insert(key).second) {
      specs.push_back(std::move(spec));
    }
  }

  return specs;
}

void require_dpor_matches_oracle(const Program& program, const std::string& description) {
  const auto oracle_signatures = collect_oracle_signatures(program);

  std::vector<std::string> dpor_observed;
  std::set<std::string> dpor_unique;
  AsyncConsistencyChecker checker;
  bool found_inconsistent_graph = false;

  DporConfig config;
  config.program = program;
  config.on_execution = [&](const ExplorationGraph& graph) {
    const auto consistency = checker.check(graph.execution_graph());
    if (!consistency.is_consistent()) {
      found_inconsistent_graph = true;
    }
    const auto sig = graph_signature(graph);
    dpor_observed.push_back(sig);
    dpor_unique.insert(sig);
  };

  const auto result = verify(config);

  std::set<std::string> missing_from_dpor;
  std::set<std::string> unexpected_in_dpor;
  std::set_difference(
      oracle_signatures.begin(),
      oracle_signatures.end(),
      dpor_unique.begin(),
      dpor_unique.end(),
      std::inserter(missing_from_dpor, missing_from_dpor.end()));
  std::set_difference(
      dpor_unique.begin(),
      dpor_unique.end(),
      oracle_signatures.begin(),
      oracle_signatures.end(),
      std::inserter(unexpected_in_dpor, unexpected_in_dpor.end()));

  INFO("stress program: " << description);
  INFO("oracle signatures: " << oracle_signatures.size());
  INFO("dpor executions_explored: " << result.executions_explored);
  INFO("dpor unique signatures: " << dpor_unique.size());
  INFO("missing signatures: " << missing_from_dpor.size());
  INFO("unexpected signatures: " << unexpected_in_dpor.size());

  REQUIRE_FALSE(found_inconsistent_graph);
  REQUIRE(result.kind == VerifyResultKind::AllExecutionsExplored);
  REQUIRE(dpor_unique.size() == dpor_observed.size());
  REQUIRE(dpor_unique == oracle_signatures);
}
}  // namespace

TEST_CASE("Algorithm 1 stress: DPOR matches independent oracle on generated async programs",
    "[algo][dpor][stress][paper]") {
  const auto specs = generate_stress_specs(72);
  REQUIRE(specs.size() == 72);

  for (const auto& spec : specs) {
    const auto program = build_program_from_spec(spec);
    require_dpor_matches_oracle(program, spec_to_string(spec));
  }
}

TEST_CASE("Algorithm 1 stress: trace-dependent receive predicates remain complete and sound",
    "[algo][dpor][stress][paper]") {
  Program program;

  // T1: nd({a,b}); send(T3, choice)
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "a",
          .choices = {"a", "b"},
      };
    }
    if (step == 1 && trace.size() == 1) {
      return SendLabel{
          .destination = 3,
          .value = trace[0],
      };
    }
    return std::nullopt;
  };

  // T2: send(T3, b); send(T3, a)
  program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{
          .destination = 3,
          .value = "b",
      };
    }
    if (step == 1) {
      return SendLabel{
          .destination = 3,
          .value = "a",
      };
    }
    return std::nullopt;
  };

  // T3: recv(any); recv(x == first_received_value)
  program.threads[3] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_receive_label<Value>();
    }
    if (step == 1 && trace.size() == 1) {
      return make_receive_label<Value>(
          [expected = trace[0]](const Value& candidate) {
            return candidate == expected;
          });
    }
    return std::nullopt;
  };

  require_dpor_matches_oracle(
      program,
      "T1=[ND({a,b}),S(3,trace[0])]; T2=[S(3,b),S(3,a)]; T3=[R(*),R(x==trace[0])]");
}

TEST_CASE("Algorithm 1 fuzz: DPOR matches oracle across many random seeds",
    "[algo][dpor][fuzz][paper]") {
  constexpr std::size_t kRounds = 20;
  constexpr std::size_t kSpecsPerRound = 36;

  for (std::size_t round = 0; round < kRounds; ++round) {
    const auto seed = static_cast<std::uint32_t>(round * 0x9E3779B9u);
    const auto specs = generate_stress_specs(kSpecsPerRound, seed);

    INFO("fuzz round " << round << " (seed=0x" << std::hex << seed << std::dec
         << ", specs=" << specs.size() << ")");
    REQUIRE(specs.size() <= kSpecsPerRound);

    for (const auto& spec : specs) {
      const auto program = build_program_from_spec(spec);
      require_dpor_matches_oracle(program, spec_to_string(spec));
    }
  }
}
