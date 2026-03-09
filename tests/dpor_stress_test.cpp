#include "dpor/algo/dpor.hpp"
#include "dpor/model/consistency.hpp"

#include "support/oracle.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using namespace dpor::algo;
using namespace dpor::model;
using dpor::test_support::require_dpor_matches_oracle;

enum class ScriptOpKind : std::uint8_t {
  SendFixed,
  SendFromLastTrace,
  ReceiveAny,
  ReceiveValues,
  ReceiveLastTraceValue,
  NondeterministicChoice,
};

struct ScriptOp {
  ScriptOpKind kind{ScriptOpKind::SendFixed};
  ThreadId destination{0};
  Value value;
  std::vector<Value> values;
  ReceiveMode receive_mode{ReceiveMode::Blocking};
};

using ThreadScript = std::vector<ScriptOp>;
using ProgramSpec = std::map<ThreadId, ThreadScript>;

Program build_program_from_spec(const ProgramSpec& spec) {
  Program program;

  for (const auto& [tid, script] : spec) {
    program.threads[tid] = [script](const ThreadTrace& trace,
                                    std::size_t step) -> std::optional<EventLabel> {
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
          if (trace.back().is_bottom()) {
            throw std::logic_error("SendFromLastTrace requires a concrete prior observation");
          }
          return SendLabel{
              .destination = op.destination,
              .value = trace.back().value(),
          };
        case ScriptOpKind::ReceiveAny:
          return make_receive_label<Value>(match_any_value<Value>(), op.receive_mode);
        case ScriptOpKind::ReceiveValues:
          return make_receive_label_from_values<Value>(op.values, op.receive_mode);
        case ScriptOpKind::ReceiveLastTraceValue:
          if (trace.empty()) {
            return std::nullopt;
          }
          return make_receive_label<Value>(
              [expected = trace.back()](const Value& candidate) { return candidate == expected; },
              op.receive_mode);
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
      }
      return std::nullopt;
    };
  }

  return program;
}

std::string script_op_to_string(const ScriptOp& op) {
  std::ostringstream oss;
  const auto* const receive_prefix = op.receive_mode == ReceiveMode::NonBlocking ? "Rnb" : "Rb";
  switch (op.kind) {
    case ScriptOpKind::SendFixed:
      oss << "S(" << op.destination << "," << op.value << ")";
      break;
    case ScriptOpKind::SendFromLastTrace:
      oss << "S(" << op.destination << ",trace[-1])";
      break;
    case ScriptOpKind::ReceiveAny:
      oss << receive_prefix << "(*)";
      break;
    case ScriptOpKind::ReceiveValues:
      oss << receive_prefix << "({";
      for (std::size_t i = 0; i < op.values.size(); ++i) {
        if (i != 0) {
          oss << ",";
        }
        oss << op.values[i];
      }
      oss << "})";
      break;
    case ScriptOpKind::ReceiveLastTraceValue:
      oss << receive_prefix << "(trace[-1])";
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

std::vector<ProgramSpec> generate_stress_specs(std::size_t target_count,
                                               std::uint32_t seed = 0xC0FFEEU) {
  std::vector<ProgramSpec> specs;
  std::set<std::string> seen_specs;

  constexpr std::array<const char*, 3> kValues = {"a", "b", "c"};
  std::mt19937 rng(seed);
  std::uniform_int_distribution<int> receiver_dist(1, 3);
  std::uniform_int_distribution<int> value_dist(0, 2);
  std::uniform_int_distribution<int> receiver_pattern_dist(0, 7);
  std::bernoulli_distribution coin_flip(0.5);
  std::bernoulli_distribution one_in_three(1.0 / 3.0);
  std::bernoulli_distribution nonblocking_receive(0.25);

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
    // Keep at most one non-blocking receive per generated spec so the oracle
    // remains cheap while still exercising nb-receive semantics.
    bool used_nonblocking_receive = false;
    const auto choose_receive_mode = [&](const bool allow_nonblocking = true) {
      if (!allow_nonblocking || used_nonblocking_receive || !nonblocking_receive(rng)) {
        return ReceiveMode::Blocking;
      }
      used_nonblocking_receive = true;
      return ReceiveMode::NonBlocking;
    };

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
            .receive_mode = choose_receive_mode(),
        });
        break;
      case 1:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v1},
            .receive_mode = choose_receive_mode(),
        });
        break;
      case 2:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v2},
            .receive_mode = choose_receive_mode(),
        });
        break;
      case 3:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveAny,
            .receive_mode = choose_receive_mode(),
        });
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveAny,
            .receive_mode = choose_receive_mode(),
        });
        break;
      case 4:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v1},
            .receive_mode = choose_receive_mode(),
        });
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v2},
            .receive_mode = choose_receive_mode(),
        });
        break;
      case 5:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v2},
            .receive_mode = choose_receive_mode(),
        });
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v1},
            .receive_mode = choose_receive_mode(),
        });
        break;
      case 6:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveValues,
            .values = {v1, v2},
            .receive_mode = choose_receive_mode(),
        });
        break;
      case 7:
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveAny,
            .receive_mode = choose_receive_mode(),
        });
        receiver_script.push_back(ScriptOp{
            .kind = ScriptOpKind::ReceiveLastTraceValue,
            .receive_mode = choose_receive_mode(),
        });
        break;
      default:
        break;
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

bool spec_uses_nonblocking_receive(const ProgramSpec& spec) {
  for (const auto& [_, script] : spec) {
    for (const auto& op : script) {
      if ((op.kind == ScriptOpKind::ReceiveAny || op.kind == ScriptOpKind::ReceiveValues ||
           op.kind == ScriptOpKind::ReceiveLastTraceValue) &&
          op.receive_mode == ReceiveMode::NonBlocking) {
        return true;
      }
    }
  }
  return false;
}
}  // namespace

TEST_CASE("Algorithm 1 stress: DPOR matches independent oracle on generated async programs",
          "[algo][dpor][stress][paper]") {
  const auto specs = generate_stress_specs(72);
  REQUIRE(specs.size() == 72);
  REQUIRE(std::any_of(specs.begin(), specs.end(), spec_uses_nonblocking_receive));

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
          .value = trace[0].value(),
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
          [expected = trace[0].value()](const Value& candidate) { return candidate == expected; });
    }
    return std::nullopt;
  };

  require_dpor_matches_oracle(
      program, "T1=[ND({a,b}),S(3,trace[0])]; T2=[S(3,b),S(3,a)]; T3=[R(*),R(x==trace[0])]");
}

TEST_CASE("Algorithm 1 stress: non-blocking receives expose bottom in traces",
          "[algo][dpor][stress][paper][nonblocking]") {
  Program program;

  // T1: recv_nb(*); send(T3, timeout) on bottom, otherwise forward the value.
  program.threads[1] = [](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0 && trace.empty()) {
      return make_nonblocking_receive_label<Value>();
    }
    if (step == 1 && trace.size() == 1) {
      if (trace[0].is_bottom()) {
        return SendLabel{
            .destination = 3,
            .value = "timeout",
        };
      }
      return SendLabel{
          .destination = 3,
          .value = trace[0].value(),
      };
    }
    return std::nullopt;
  };

  // T2: send(T1, x)
  program.threads[2] = [](const ThreadTrace&, std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{
          .destination = 1,
          .value = "x",
      };
    }
    return std::nullopt;
  };

  // T3: recv({timeout, x})
  program.threads[3] = [](const ThreadTrace& trace, std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label_from_values<Value>({"timeout", "x"});
    }
    return std::nullopt;
  };

  require_dpor_matches_oracle(
      program, "T1=[Rnb(*),S(3,bottom?timeout:trace[0])]; T2=[S(1,x)]; T3=[Rb({timeout,x})]");
}

TEST_CASE("Algorithm 1 fuzz: DPOR matches oracle across many random seeds",
          "[algo][dpor][fuzz][paper]") {
  constexpr std::size_t kRounds = 20;
  constexpr std::size_t kSpecsPerRound = 36;

  for (std::size_t round = 0; round < kRounds; ++round) {
    const auto seed = static_cast<std::uint32_t>(round * 0x9E3779B9U);
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
