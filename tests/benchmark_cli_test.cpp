#include "two_phase_commit_benchmark.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using dpor::algo::Program;
using dpor::algo::ThreadTrace;
using dpor::benchmarks::detail::Options;
using dpor::model::CommunicationModel;
using dpor::model::EventLabel;
using dpor::model::SendLabel;
using dpor::model::Value;
using dpor::model::make_receive_label;

[[nodiscard]] Options parse_options(std::initializer_list<std::string_view> args) {
  std::vector<std::string> storage;
  storage.reserve(args.size() + 1);
  storage.emplace_back("benchmark");
  for (const auto arg : args) {
    storage.emplace_back(arg);
  }

  std::vector<char*> argv;
  argv.reserve(storage.size());
  for (auto& arg : storage) {
    argv.push_back(arg.data());
  }

  return dpor::benchmarks::detail::parse_args(static_cast<int>(argv.size()), argv.data(),
                                              "benchmark");
}

[[nodiscard]] Program make_fifo_sensitive_program() {
  Program program;

  program.threads[1] = [](const ThreadTrace&,
                          const std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "a"};
    }
    if (step == 1) {
      return SendLabel{.destination = 2, .value = "b"};
    }
    return std::nullopt;
  };

  program.threads[2] = [](const ThreadTrace& trace,
                          const std::size_t) -> std::optional<EventLabel> {
    if (trace.empty()) {
      return make_receive_label<Value>();
    }
    return std::nullopt;
  };

  return program;
}

}  // namespace

TEST_CASE("benchmark CLI defaults to async communication model", "[benchmarks][cli]") {
  const auto options = parse_options({});

  REQUIRE(options.communication_model == CommunicationModel::Async);
}

TEST_CASE("benchmark CLI accepts --fifo", "[benchmarks][cli]") {
  const auto options = parse_options({"--fifo"});

  REQUIRE(options.communication_model == CommunicationModel::FifoP2P);
}

TEST_CASE("benchmark CLI accepts --progress-interval-ms", "[benchmarks][cli]") {
  const auto options = parse_options({"--progress-interval-ms", "250"});

  REQUIRE(options.progress_report_interval == std::chrono::milliseconds(250));
}

TEST_CASE("benchmark CLI accepts --progress-counter-flush-interval", "[benchmarks][cli]") {
  const auto options = parse_options({"--progress-counter-flush-interval", "4096"});

  REQUIRE(options.parallel);
  REQUIRE(options.parallel_options.progress_counter_flush_interval == 4096);
}

TEST_CASE("benchmark helper forwards communication model to DPOR and oracle",
          "[benchmarks][cli]") {
  Options async_options;
  async_options.participants = 2;
  async_options.inject_crash = false;
  async_options.progress_report_interval = std::chrono::milliseconds::zero();

  Options fifo_options = async_options;
  fifo_options.communication_model = CommunicationModel::FifoP2P;

  const auto make_program = [](std::size_t, bool) { return make_fifo_sensitive_program(); };

  const auto async_dpor = dpor::benchmarks::detail::run_dpor(async_options, make_program, 1);
  const auto fifo_dpor = dpor::benchmarks::detail::run_dpor(fifo_options, make_program, 1);
  const auto async_oracle = dpor::benchmarks::detail::run_oracle(
      async_options.participants, async_options.inject_crash, async_options.communication_model,
      make_program);
  const auto fifo_oracle = dpor::benchmarks::detail::run_oracle(
      fifo_options.participants, fifo_options.inject_crash, fifo_options.communication_model,
      make_program);

  REQUIRE(async_dpor.terminal_executions == 2);
  REQUIRE(async_dpor.full_executions == 2);
  REQUIRE(async_dpor.error_executions == 0);
  REQUIRE(async_dpor.depth_limit_executions == 0);

  REQUIRE(fifo_dpor.terminal_executions == 1);
  REQUIRE(fifo_dpor.full_executions == 1);
  REQUIRE(fifo_dpor.error_executions == 0);
  REQUIRE(fifo_dpor.depth_limit_executions == 0);

  REQUIRE(async_oracle.terminal_executions == 2);
  REQUIRE(async_oracle.full_executions == 2);
  REQUIRE(async_oracle.error_executions == 0);
  REQUIRE(async_oracle.depth_limit_executions == 0);

  REQUIRE(fifo_oracle.terminal_executions == 1);
  REQUIRE(fifo_oracle.full_executions == 1);
  REQUIRE(fifo_oracle.error_executions == 0);
  REQUIRE(fifo_oracle.depth_limit_executions == 0);
}
