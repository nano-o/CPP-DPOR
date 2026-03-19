#pragma once

#include "dpor/algo/dpor.hpp"

#include "support/oracle_core.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace dpor::benchmarks {

namespace detail {

struct Options {
  enum class Mode { Dpor, Oracle, Both };

  Mode mode{Mode::Both};
  model::CommunicationModel communication_model{model::CommunicationModel::Async};
  std::size_t participants{3};
  std::size_t iterations{1};
  bool inject_crash{true};
  bool parallel{false};
  std::chrono::milliseconds progress_report_interval{std::chrono::seconds(1)};
  algo::ParallelVerifyOptions parallel_options{};
};

struct Measurement {
  std::size_t terminal_executions{0};
  std::size_t full_executions{0};
  std::size_t error_executions{0};
  std::size_t depth_limit_executions{0};
  std::size_t paths_explored{0};
  double elapsed_ms{0.0};
};

struct BenchmarkRunResult {
  std::size_t terminal_executions{0};
  std::size_t full_executions{0};
  std::size_t error_executions{0};
  std::size_t depth_limit_executions{0};
  std::size_t paths_explored{0};
};

template <typename ProgramT>
struct ProgramValueType;

template <typename ValueT>
struct ProgramValueType<algo::ProgramT<ValueT>> {
  using type = ValueT;
};

[[nodiscard]] inline std::string_view communication_model_name(
    const model::CommunicationModel communication_model) {
  switch (communication_model) {
    case model::CommunicationModel::Async:
      return "async";
    case model::CommunicationModel::FifoP2P:
      return "fifo_p2p";
  }
  throw std::logic_error("unknown communication model");
}

[[noreturn]] inline void print_usage_and_exit(const char* argv0, std::string_view benchmark_label,
                                              int exit_code) {
  std::ostream& os = exit_code == 0 ? std::cout : std::cerr;
  os << "Usage: " << argv0
     << " [--mode dpor|oracle|both] [--participants N] [--iterations N]"
        " [--no-crash] [--fifo] [--parallel]"
        " [--max-workers N] [--max-queued-tasks N]"
        " [--spawn-depth-cutoff N] [--min-fanout N]"
        " [--progress-counter-flush-interval N]"
        " [--progress-poll-interval-steps N]"
        " [--progress-interval-ms N]\n";
  os << benchmark_label << '\n';
  std::exit(exit_code);
}

[[nodiscard]] inline std::size_t parse_positive_int(std::string_view text, std::string_view flag) {
  if (!text.empty() && text.front() == '-') {
    throw std::invalid_argument("invalid numeric value for " + std::string(flag));
  }
  std::size_t value = 0;
  try {
    value = static_cast<std::size_t>(std::stoull(std::string{text}));
  } catch (const std::exception&) {
    throw std::invalid_argument("invalid numeric value for " + std::string(flag));
  }
  if (value == 0) {
    throw std::invalid_argument(std::string(flag) + " must be positive");
  }
  return value;
}

[[nodiscard]] inline std::size_t parse_nonnegative_int(std::string_view text,
                                                       std::string_view flag) {
  if (!text.empty() && text.front() == '-') {
    throw std::invalid_argument("invalid numeric value for " + std::string(flag));
  }
  try {
    return static_cast<std::size_t>(std::stoull(std::string{text}));
  } catch (const std::exception&) {
    throw std::invalid_argument("invalid numeric value for " + std::string(flag));
  }
}

[[nodiscard]] inline Options parse_args(int argc, char** argv, std::string_view benchmark_label) {
  Options options;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      print_usage_and_exit(argv[0], benchmark_label, 0);
    }
    if (arg == "--no-crash") {
      options.inject_crash = false;
      continue;
    }
    if (arg == "--fifo") {
      options.communication_model = model::CommunicationModel::FifoP2P;
      continue;
    }
    if (arg == "--parallel") {
      options.parallel = true;
      continue;
    }
    if (i + 1 >= argc) {
      throw std::invalid_argument("missing value for " + std::string(arg));
    }

    const std::string_view value = argv[++i];
    if (arg == "--mode") {
      if (value == "dpor") {
        options.mode = Options::Mode::Dpor;
      } else if (value == "oracle") {
        options.mode = Options::Mode::Oracle;
      } else if (value == "both") {
        options.mode = Options::Mode::Both;
      } else {
        throw std::invalid_argument("invalid mode: " + std::string(value));
      }
      continue;
    }
    if (arg == "--participants") {
      options.participants = parse_positive_int(value, arg);
      continue;
    }
    if (arg == "--iterations") {
      options.iterations = parse_positive_int(value, arg);
      continue;
    }
    if (arg == "--max-workers") {
      options.parallel = true;
      options.parallel_options.max_workers = parse_positive_int(value, arg);
      continue;
    }
    if (arg == "--max-queued-tasks") {
      options.parallel = true;
      options.parallel_options.max_queued_tasks = parse_nonnegative_int(value, arg);
      continue;
    }
    if (arg == "--spawn-depth-cutoff") {
      options.parallel = true;
      options.parallel_options.spawn_depth_cutoff = parse_nonnegative_int(value, arg);
      continue;
    }
    if (arg == "--min-fanout") {
      options.parallel = true;
      options.parallel_options.min_fanout = parse_nonnegative_int(value, arg);
      continue;
    }
    if (arg == "--progress-interval-ms") {
      options.progress_report_interval =
          std::chrono::milliseconds(parse_nonnegative_int(value, arg));
      continue;
    }
    if (arg == "--progress-counter-flush-interval") {
      options.parallel = true;
      options.parallel_options.progress_counter_flush_interval = parse_nonnegative_int(value, arg);
      continue;
    }
    if (arg == "--progress-poll-interval-steps") {
      options.parallel = true;
      options.parallel_options.progress_poll_interval_steps = parse_nonnegative_int(value, arg);
      continue;
    }

    throw std::invalid_argument("unknown argument: " + std::string(arg));
  }

  return options;
}

template <typename Fn>
[[nodiscard]] inline Measurement measure_once(Fn&& fn) {
  const auto start = std::chrono::steady_clock::now();
  const auto result = fn();
  const auto end = std::chrono::steady_clock::now();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start);
  return Measurement{
      .terminal_executions = result.terminal_executions,
      .full_executions = result.full_executions,
      .error_executions = result.error_executions,
      .depth_limit_executions = result.depth_limit_executions,
      .paths_explored = result.paths_explored,
      .elapsed_ms = elapsed.count(),
  };
}

inline void print_execution_counts(const Measurement& measurement) {
  std::cout << " terminal_executions=" << measurement.terminal_executions
            << " full_executions=" << measurement.full_executions
            << " error_executions=" << measurement.error_executions
            << " depth_limit_executions=" << measurement.depth_limit_executions;
}

inline void print_progress_snapshot(const std::size_t run_index,
                                    const algo::ProgressSnapshot& snapshot) {
  if (snapshot.state != algo::ProgressState::Running) {
    return;
  }

  const auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(snapshot.elapsed);
  std::cout << "  progress run " << run_index << ":" << " elapsed_ms=" << std::fixed
            << std::setprecision(3) << elapsed_ms.count()
            << " terminal_executions=" << snapshot.terminal_executions
            << " full_executions=" << snapshot.full_executions
            << " error_executions=" << snapshot.error_executions
            << " depth_limit_executions=" << snapshot.depth_limit_executions
            << " active_workers=" << snapshot.active_workers << "/" << snapshot.max_workers
            << " queued_tasks=" << snapshot.queued_tasks << "/" << snapshot.max_queued_tasks
            << " counts_exact=" << std::boolalpha << snapshot.counts_exact << '\n'
            << std::flush;
}

inline void print_measurements(std::string_view label, const std::vector<Measurement>& measurements,
                               bool show_paths) {
  if (measurements.empty()) {
    return;
  }

  const auto minmax = std::minmax_element(measurements.begin(), measurements.end(),
                                          [](const Measurement& lhs, const Measurement& rhs) {
                                            return lhs.elapsed_ms < rhs.elapsed_ms;
                                          });
  const auto total_ms = std::accumulate(
      measurements.begin(), measurements.end(), 0.0,
      [](double sum, const Measurement& measurement) { return sum + measurement.elapsed_ms; });
  const auto average_ms = total_ms / static_cast<double>(measurements.size());

  std::cout << label << '\n';
  for (std::size_t i = 0; i < measurements.size(); ++i) {
    std::cout << "  run " << (i + 1) << ":";
    print_execution_counts(measurements[i]);
    if (show_paths) {
      std::cout << " paths_explored=" << measurements[i].paths_explored;
    }
    std::cout << " elapsed_ms=" << std::fixed << std::setprecision(3) << measurements[i].elapsed_ms
              << '\n';
  }
  std::cout << "  summary:" << " min_ms=" << std::fixed << std::setprecision(3)
            << minmax.first->elapsed_ms << " avg_ms=" << average_ms
            << " max_ms=" << minmax.second->elapsed_ms;
  print_execution_counts(measurements.front());
  if (show_paths) {
    std::cout << " paths_explored=" << measurements.front().paths_explored;
  }
  std::cout << '\n';
}

template <typename ProgramFactory>
[[nodiscard]] inline BenchmarkRunResult run_dpor(const Options& options,
                                                 const ProgramFactory& make_program,
                                                 const std::size_t run_index) {
  auto program = make_program(options.participants, options.inject_crash);
  using ValueT = typename ProgramValueType<std::decay_t<decltype(program)>>::type;
  algo::DporConfigT<ValueT> config;
  config.program = std::move(program);
  config.communication_model = options.communication_model;
  config.progress_report_interval = options.progress_report_interval;
  if (options.progress_report_interval > std::chrono::milliseconds::zero()) {
    config.on_progress = [run_index](const algo::ProgressSnapshot& snapshot) {
      print_progress_snapshot(run_index, snapshot);
    };
  }
  const auto result = options.parallel ? algo::verify_parallel(config, options.parallel_options)
                                       : algo::verify(config);
  if (result.kind != algo::VerifyResultKind::AllExplored) {
    throw std::runtime_error("DPOR was stopped before exploring all executions");
  }
  return BenchmarkRunResult{
      .terminal_executions = result.executions_explored,
      .full_executions = result.full_executions_explored,
      .error_executions = result.error_executions_explored,
      .depth_limit_executions = result.depth_limit_executions_explored,
      .paths_explored = 0,
  };
}

template <typename ProgramFactory>
[[nodiscard]] inline BenchmarkRunResult run_oracle(std::size_t participants, bool inject_crash,
                                                   model::CommunicationModel communication_model,
                                                   const ProgramFactory& make_program) {
  auto program = make_program(participants, inject_crash);
  const auto stats = test_support::collect_oracle_stats(program, communication_model);
  return BenchmarkRunResult{
      .terminal_executions = stats.signatures.size(),
      .full_executions = stats.signatures.size(),
      .error_executions = 0,
      .depth_limit_executions = 0,
      .paths_explored = stats.paths_explored,
  };
}

template <typename ProgramFactory>
[[nodiscard]] inline std::vector<Measurement> collect_measurements(
    const Options& options, bool use_oracle, const ProgramFactory& make_program) {
  std::vector<Measurement> measurements;
  measurements.reserve(options.iterations);

  for (std::size_t i = 0; i < options.iterations; ++i) {
    measurements.push_back(
        use_oracle ? measure_once([&] {
          return run_oracle(options.participants, options.inject_crash, options.communication_model,
                            make_program);
        })
                   : measure_once([&] { return run_dpor(options, make_program, i + 1); }));
    if (measurements.back().terminal_executions != measurements.front().terminal_executions) {
      throw std::runtime_error("terminal execution count changed across iterations");
    }
    if (measurements.back().full_executions != measurements.front().full_executions) {
      throw std::runtime_error("full execution count changed across iterations");
    }
    if (measurements.back().error_executions != measurements.front().error_executions) {
      throw std::runtime_error("error execution count changed across iterations");
    }
    if (measurements.back().depth_limit_executions !=
        measurements.front().depth_limit_executions) {
      throw std::runtime_error("depth-limit execution count changed across iterations");
    }
    if (measurements.back().paths_explored != measurements.front().paths_explored) {
      throw std::runtime_error("path count changed across iterations");
    }
  }

  return measurements;
}

}  // namespace detail

template <typename ProgramFactory>
inline int run_two_phase_commit_benchmark(int argc, char** argv, std::string_view benchmark_label,
                                          const ProgramFactory& make_program) {
  try {
    const auto options = detail::parse_args(argc, argv, benchmark_label);

    std::cout << benchmark_label << " participants=" << options.participants
              << " communication_model="
              << detail::communication_model_name(options.communication_model)
              << " inject_crash=" << std::boolalpha << options.inject_crash
              << " iterations=" << options.iterations;
    if (options.parallel && options.mode != detail::Options::Mode::Oracle) {
      std::cout << " parallel=true" << " max_workers=" << options.parallel_options.max_workers
                << " max_queued_tasks=" << options.parallel_options.max_queued_tasks
                << " spawn_depth_cutoff=" << options.parallel_options.spawn_depth_cutoff
                << " min_fanout=" << options.parallel_options.min_fanout
                << " progress_counter_flush_interval="
                << options.parallel_options.progress_counter_flush_interval
                << " progress_poll_interval_steps="
                << options.parallel_options.progress_poll_interval_steps;
    }
    if (options.mode != detail::Options::Mode::Oracle) {
      std::cout << " progress_interval_ms=" << options.progress_report_interval.count();
    }
#ifdef NDEBUG
    std::cout << " optimized_build=true\n";
#else
    std::cout << " optimized_build=false\n";
#endif

    std::vector<detail::Measurement> dpor_measurements;
    std::vector<detail::Measurement> oracle_measurements;

    if (options.parallel && options.mode == detail::Options::Mode::Oracle) {
      std::cerr << "warning: parallel options are ignored in oracle-only mode\n";
    }

    if (options.mode == detail::Options::Mode::Dpor ||
        options.mode == detail::Options::Mode::Both) {
      dpor_measurements = detail::collect_measurements(options, false, make_program);
      detail::print_measurements("DPOR", dpor_measurements, false);
    }

    if (options.mode == detail::Options::Mode::Oracle ||
        options.mode == detail::Options::Mode::Both) {
      oracle_measurements = detail::collect_measurements(options, true, make_program);
      detail::print_measurements("Oracle", oracle_measurements, true);
    }

    if (!dpor_measurements.empty() && !oracle_measurements.empty() &&
        dpor_measurements.front().full_executions != oracle_measurements.front().full_executions) {
      std::cerr << "full execution count mismatch: dpor="
                << dpor_measurements.front().full_executions
                << " oracle=" << oracle_measurements.front().full_executions << '\n';
      return 2;
    }

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }
}

}  // namespace dpor::benchmarks
