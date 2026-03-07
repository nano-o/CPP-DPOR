#pragma once

#include "support/oracle_core.hpp"

#include "dpor/algo/dpor.hpp"

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
  std::size_t participants{3};
  std::size_t iterations{1};
  bool inject_crash{true};
};

struct Measurement {
  std::size_t executions{0};
  std::size_t paths_explored{0};
  double elapsed_ms{0.0};
};

struct OracleRunResult {
  std::size_t executions{0};
  std::size_t paths_explored{0};
};

template <typename ProgramT>
struct ProgramValueType;

template <typename ValueT>
struct ProgramValueType<algo::ProgramT<ValueT>> {
  using type = ValueT;
};

[[noreturn]] inline void print_usage_and_exit(
    const char* argv0,
    std::string_view benchmark_label,
    int exit_code) {
  std::ostream& os = exit_code == 0 ? std::cout : std::cerr;
  os << "Usage: " << argv0
     << " [--mode dpor|oracle|both] [--participants N] [--iterations N]"
        " [--no-crash]\n";
  os << benchmark_label << '\n';
  std::exit(exit_code);
}

[[nodiscard]] inline std::size_t parse_positive_int(
    std::string_view text,
    std::string_view flag) {
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

[[nodiscard]] inline Options parse_args(
    int argc,
    char** argv,
    std::string_view benchmark_label) {
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
      .executions = result.executions,
      .paths_explored = result.paths_explored,
      .elapsed_ms = elapsed.count(),
  };
}

inline void print_measurements(
    std::string_view label,
    const std::vector<Measurement>& measurements,
    bool show_paths) {
  if (measurements.empty()) {
    return;
  }

  const auto minmax = std::minmax_element(
      measurements.begin(),
      measurements.end(),
      [](const Measurement& lhs, const Measurement& rhs) {
        return lhs.elapsed_ms < rhs.elapsed_ms;
      });
  const auto total_ms = std::accumulate(
      measurements.begin(), measurements.end(), 0.0,
      [](double sum, const Measurement& measurement) {
        return sum + measurement.elapsed_ms;
      });
  const auto average_ms = total_ms / static_cast<double>(measurements.size());

  std::cout << label << '\n';
  for (std::size_t i = 0; i < measurements.size(); ++i) {
    std::cout << "  run " << (i + 1)
              << ": executions=" << measurements[i].executions;
    if (show_paths) {
      std::cout << " paths_explored=" << measurements[i].paths_explored;
    }
    std::cout << " elapsed_ms=" << std::fixed << std::setprecision(3)
              << measurements[i].elapsed_ms << '\n';
  }
  std::cout << "  summary:"
            << " min_ms=" << std::fixed << std::setprecision(3)
            << minmax.first->elapsed_ms
            << " avg_ms=" << average_ms
            << " max_ms=" << minmax.second->elapsed_ms
            << " executions=" << measurements.front().executions;
  if (show_paths) {
    std::cout << " paths_explored=" << measurements.front().paths_explored;
  }
  std::cout << '\n';
}

template <typename ProgramFactory>
[[nodiscard]] inline OracleRunResult run_dpor(
    std::size_t participants,
    bool inject_crash,
    const ProgramFactory& make_program) {
  auto program = make_program(participants, inject_crash);
  using ValueT =
      typename ProgramValueType<std::decay_t<decltype(program)>>::type;
  algo::DporConfigT<ValueT> config;
  config.program = std::move(program);
  const auto result = algo::verify(config);
  if (result.kind != algo::VerifyResultKind::AllExecutionsExplored) {
    throw std::runtime_error("DPOR did not explore all executions");
  }
  return OracleRunResult{
      .executions = result.executions_explored,
      .paths_explored = 0,
  };
}

template <typename ProgramFactory>
[[nodiscard]] inline OracleRunResult run_oracle(
    std::size_t participants,
    bool inject_crash,
    const ProgramFactory& make_program) {
  auto program = make_program(participants, inject_crash);
  const auto stats = test_support::collect_oracle_stats(program);
  return OracleRunResult{
      .executions = stats.signatures.size(),
      .paths_explored = stats.paths_explored,
  };
}

template <typename ProgramFactory>
[[nodiscard]] inline std::vector<Measurement> collect_measurements(
    std::size_t iterations,
    std::size_t participants,
    bool inject_crash,
    bool use_oracle,
    const ProgramFactory& make_program) {
  std::vector<Measurement> measurements;
  measurements.reserve(iterations);

  for (std::size_t i = 0; i < iterations; ++i) {
    measurements.push_back(
        use_oracle
            ? measure_once([&] {
                return run_oracle(participants, inject_crash, make_program);
              })
            : measure_once([&] {
                return run_dpor(participants, inject_crash, make_program);
              }));
    if (measurements.back().executions != measurements.front().executions) {
      throw std::runtime_error("execution count changed across iterations");
    }
    if (measurements.back().paths_explored != measurements.front().paths_explored) {
      throw std::runtime_error("path count changed across iterations");
    }
  }

  return measurements;
}

}  // namespace detail

template <typename ProgramFactory>
inline int run_two_phase_commit_benchmark(
    int argc,
    char** argv,
    std::string_view benchmark_label,
    const ProgramFactory& make_program) {
  try {
    const auto options = detail::parse_args(argc, argv, benchmark_label);

    std::cout << benchmark_label
              << " participants=" << options.participants
              << " inject_crash=" << std::boolalpha << options.inject_crash
              << " iterations=" << options.iterations;
#ifdef NDEBUG
    std::cout << " optimized_build=true\n";
#else
    std::cout << " optimized_build=false\n";
#endif

    std::vector<detail::Measurement> dpor_measurements;
    std::vector<detail::Measurement> oracle_measurements;

    if (options.mode == detail::Options::Mode::Dpor ||
        options.mode == detail::Options::Mode::Both) {
      dpor_measurements = detail::collect_measurements(
          options.iterations,
          options.participants,
          options.inject_crash,
          false,
          make_program);
      detail::print_measurements("DPOR", dpor_measurements, false);
    }

    if (options.mode == detail::Options::Mode::Oracle ||
        options.mode == detail::Options::Mode::Both) {
      oracle_measurements = detail::collect_measurements(
          options.iterations,
          options.participants,
          options.inject_crash,
          true,
          make_program);
      detail::print_measurements("Oracle", oracle_measurements, true);
    }

    if (!dpor_measurements.empty() && !oracle_measurements.empty() &&
        dpor_measurements.front().executions != oracle_measurements.front().executions) {
      std::cerr << "execution count mismatch: dpor="
                << dpor_measurements.front().executions
                << " oracle=" << oracle_measurements.front().executions << '\n';
      return 2;
    }

    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }
}

}  // namespace dpor::benchmarks
