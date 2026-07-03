#include "dpor/errors.hpp"

#include "dpor/algo/dpor.hpp"
#include "dpor/model/consistency.hpp"
#include "dpor/model/format.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using namespace dpor::algo;
using namespace dpor::model;

[[nodiscard]] bool contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

template <typename Fn>
[[nodiscard]] std::optional<dpor::user_code_error> catch_user_code_error(Fn&& fn) {
  try {
    fn();
  } catch (const dpor::user_code_error& err) {
    return err;
  }
  return std::nullopt;
}

}  // namespace

TEST_CASE("dpor exceptions form a catchable hierarchy", "[errors]") {
  const auto as_dpor_error = [](const auto& err) {
    try {
      throw err;
    } catch (const dpor::error& caught) {
      return std::string(caught.what());
    }
  };

  REQUIRE(as_dpor_error(dpor::internal_error("broken invariant")) == "broken invariant");
  REQUIRE(as_dpor_error(dpor::precondition_error("bad call")) == "bad call");
  REQUIRE(contains(as_dpor_error(dpor::user_code_error(dpor::UserCallbackKind::ThreadFunction, 3,
                                                       std::exception_ptr{}, "misbehaved")),
                   "thread function"));

  // dpor::error derives from std::runtime_error so generic handlers keep working.
  try {
    throw dpor::internal_error("still a std exception");
  } catch (const std::runtime_error& caught) {
    REQUIRE(std::string(caught.what()) == "still a std exception");
  }
}

TEST_CASE("user_code_error without an original exception refuses rethrow_original", "[errors]") {
  const dpor::user_code_error err(dpor::UserCallbackKind::ThreadFunction, std::nullopt,
                                  std::exception_ptr{}, "illegal result");
  REQUIRE_FALSE(err.has_original());
  REQUIRE_THROWS_AS(err.rethrow_original(), dpor::precondition_error);
}

TEST_CASE("exception escaping a thread function surfaces as user_code_error", "[errors][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    throw std::runtime_error("SUT exploded");
  };

  const auto err = catch_user_code_error([&]() { static_cast<void>(verify(config)); });
  REQUIRE(err.has_value());
  REQUIRE(err->kind() == dpor::UserCallbackKind::ThreadFunction);
  REQUIRE(err->thread() == 1U);
  REQUIRE(err->has_original());
  REQUIRE(contains(err->what(), "thread function"));
  REQUIRE(contains(err->what(), "SUT exploded"));
  REQUIRE_THROWS_AS(err->rethrow_original(), std::runtime_error);
}

TEST_CASE("exception escaping a receive matcher surfaces as user_code_error during scheduling",
          "[errors][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "ping"};
    }
    return std::nullopt;
  };
  config.program.threads[2] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return make_receive_label<Value>(
          [](const Value&) -> bool { throw std::runtime_error("matcher boom"); });
    }
    return std::nullopt;
  };

  const auto err = catch_user_code_error([&]() { static_cast<void>(verify(config)); });
  REQUIRE(err.has_value());
  REQUIRE(err->kind() == dpor::UserCallbackKind::ReceiveMatcher);
  REQUIRE(err->has_original());
  REQUIRE(contains(err->what(), "matcher boom"));
}

TEST_CASE("exception escaping a receive matcher surfaces during consistency checking",
          "[errors][consistency]") {
  ExplorationGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = graph.add_event(2, make_receive_label<Value>([](const Value&) -> bool {
                                   throw std::runtime_error("matcher boom");
                                 }));
  graph.set_reads_from(r, s);

  const AsyncConsistencyChecker checker;
  REQUIRE_THROWS_AS(checker.check(graph), dpor::user_code_error);
}

TEST_CASE("exception escaping the terminal observer surfaces as user_code_error",
          "[errors][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };
  config.on_terminal_execution = [](const ExplorationGraph&) {
    throw std::runtime_error("observer boom");
  };

  const auto err = catch_user_code_error([&]() { static_cast<void>(verify(config)); });
  REQUIRE(err.has_value());
  REQUIRE(err->kind() == dpor::UserCallbackKind::TerminalObserver);
  REQUIRE(contains(err->what(), "observer boom"));
}

TEST_CASE("exception escaping the progress observer surfaces as user_code_error",
          "[errors][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    return std::nullopt;
  };
  config.progress_report_interval = std::chrono::milliseconds::zero();
  config.on_progress = [](const ProgressSnapshot&) { throw std::runtime_error("progress boom"); };

  const auto err = catch_user_code_error([&]() { static_cast<void>(verify(config)); });
  REQUIRE(err.has_value());
  REQUIRE(err->kind() == dpor::UserCallbackKind::ProgressObserver);
}

TEST_CASE("thread function returning BlockLabel is a user_code_error without an original",
          "[errors][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return BlockLabel{};
    }
    return std::nullopt;
  };

  const auto err = catch_user_code_error([&]() { static_cast<void>(verify(config)); });
  REQUIRE(err.has_value());
  REQUIRE(err->kind() == dpor::UserCallbackKind::ThreadFunction);
  REQUIRE(err->thread() == 1U);
  REQUIRE_FALSE(err->has_original());
  REQUIRE(contains(err->what(), "BlockLabel"));
}

TEST_CASE("a user_code_error thrown by user code passes through unwrapped", "[errors][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    throw dpor::user_code_error(dpor::UserCallbackKind::ReceiveMatcher, 7, std::exception_ptr{},
                                "inner origin");
  };

  const auto err = catch_user_code_error([&]() { static_cast<void>(verify(config)); });
  REQUIRE(err.has_value());
  REQUIRE(err->kind() == dpor::UserCallbackKind::ReceiveMatcher);
  REQUIRE(err->thread() == 7U);
}

TEST_CASE("on_fatal_error observes the in-progress trace before the exception propagates",
          "[errors][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "ping"};
    }
    throw std::runtime_error("SUT exploded");
  };

  std::size_t fatal_calls = 0;
  std::size_t observed_event_count = 0;
  std::string observed_trace;
  bool exception_matches = false;
  config.on_fatal_error = [&](const FatalErrorContextT<Value>& ctx) {
    ++fatal_calls;
    observed_event_count = ctx.graph.event_count();
    observed_trace = format_graph(ctx.graph);
    try {
      std::rethrow_exception(ctx.exception);
    } catch (const dpor::user_code_error&) {
      exception_matches = true;
    } catch (...) {
    }
  };

  REQUIRE_THROWS_AS(verify(config), dpor::user_code_error);
  REQUIRE(fatal_calls == 1);
  REQUIRE(observed_event_count == 1);
  REQUIRE(contains(observed_trace, "send(dest=2, val=ping)"));
  REQUIRE(exception_matches);
}

TEST_CASE("a throwing on_fatal_error never masks the original exception", "[errors][dpor]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&, std::size_t) -> std::optional<EventLabel> {
    throw std::runtime_error("SUT exploded");
  };

  bool fatal_called = false;
  config.on_fatal_error = [&](const FatalErrorContextT<Value>&) {
    fatal_called = true;
    throw std::runtime_error("fatal observer boom");
  };

  const auto err = catch_user_code_error([&]() { static_cast<void>(verify(config)); });
  REQUIRE(err.has_value());
  REQUIRE(contains(err->what(), "SUT exploded"));
  REQUIRE(fatal_called);
}

TEST_CASE("verify_parallel wraps user exceptions and reports one fatal trace",
          "[errors][dpor][parallel]") {
  DporConfig config;
  config.program.threads[1] = [](const ThreadTrace&,
                                 std::size_t step) -> std::optional<EventLabel> {
    if (step == 0) {
      return SendLabel{.destination = 2, .value = "ping"};
    }
    throw std::runtime_error("SUT exploded");
  };

  std::atomic<std::size_t> fatal_calls{0};
  config.on_fatal_error = [&](const FatalErrorContextT<Value>&) {
    fatal_calls.fetch_add(1, std::memory_order_relaxed);
  };

  ParallelVerifyOptions options;
  options.max_workers = 2;

  const auto err =
      catch_user_code_error([&]() { static_cast<void>(verify_parallel(config, options)); });
  REQUIRE(err.has_value());
  REQUIRE(err->kind() == dpor::UserCallbackKind::ThreadFunction);
  REQUIRE(fatal_calls.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("format_graph renders every label kind in insertion order", "[errors][format]") {
  ExplorationGraph graph;
  const auto s = graph.add_event(1, SendLabel{.destination = 2, .value = "hello"});
  const auto r = graph.add_event(2, make_receive_label_from_values<Value>({"hello"}));
  graph.set_reads_from(r, s);
  const auto nb = graph.add_event(2, make_nonblocking_receive_label<Value>());
  graph.set_reads_from_bottom(nb);
  static_cast<void>(
      graph.add_event(1, NondeterministicChoiceLabel{.value = "pick-me", .choices = {}}));
  static_cast<void>(graph.add_event(3, BlockLabel{}));
  static_cast<void>(graph.add_event(3, ErrorLabel{.message = "bad state"}));

  const auto text = format_graph(graph);
  REQUIRE(contains(text, "send(dest=2, val=hello)"));
  REQUIRE(contains(text, "receive(from=0, val=hello)"));
  REQUIRE(contains(text, "receive(nonblocking, bottom)"));
  REQUIRE(contains(text, "nd(val=pick-me)"));
  REQUIRE(contains(text, "block"));
  REQUIRE(contains(text, "error(bad state)"));
}
