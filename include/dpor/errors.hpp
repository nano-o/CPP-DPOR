#pragma once

// Exception taxonomy for the dpor library.
//
// Three disjoint categories, distinguishable by type at catch sites:
// - internal_error:     a library invariant was violated; a bug in dpor itself.
// - precondition_error: the caller violated a documented API precondition.
// - user_code_error:    an exception escaped a user-provided callback (thread
//   function, receive matcher, observer), or a callback returned an illegal
//   result. The original exception, when there is one, is preserved and can be
//   rethrown for inspection.
//
// Errors detected in the system under test are NOT exceptions: harnesses report
// them by returning model::ErrorLabel from a thread function, which DPOR records
// as an error terminal execution and keeps exploring. Any exception that reaches
// verify()/verify_parallel() is fatal to the run; for exceptions raised during
// exploration, the on_fatal_error observer in DporConfigT can capture the
// in-progress execution graph for diagnostics before the exception propagates.

#include <cstdint>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>

namespace dpor {

// Base class for every exception thrown by the dpor library. Derives from
// std::runtime_error so generic catch (const std::exception&) sites keep
// working.
class error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// A library invariant was violated. This always indicates a bug in dpor itself,
// never in the harness or the system under test.
class internal_error : public error {
 public:
  using error::error;
};

// The caller violated a documented API precondition (e.g., non-compact thread
// ids, malformed reads-from edges, querying porf on a cyclic graph).
class precondition_error : public error {
 public:
  using error::error;
};

// Which user-provided callback surface a user_code_error originated from.
enum class UserCallbackKind : std::uint8_t {
  ThreadFunction,
  ReceiveMatcher,
  TerminalObserver,
  ProgressObserver,
};

[[nodiscard]] inline const char* to_string(const UserCallbackKind kind) noexcept {
  switch (kind) {
    case UserCallbackKind::ThreadFunction:
      return "thread function";
    case UserCallbackKind::ReceiveMatcher:
      return "receive matcher";
    case UserCallbackKind::TerminalObserver:
      return "terminal execution observer";
    case UserCallbackKind::ProgressObserver:
      return "progress observer";
  }
  return "unknown callback";
}

namespace detail {

[[nodiscard]] inline std::string compose_user_code_message(
    const UserCallbackKind kind, const std::optional<std::uint32_t> thread,
    const std::exception_ptr& original, const std::string& detail) {
  std::string message = "exception escaped user ";
  message += to_string(kind);
  if (thread.has_value()) {
    message += " (thread " + std::to_string(*thread) + ")";
  }
  if (!detail.empty()) {
    message += ": " + detail;
  }
  if (original) {
    try {
      std::rethrow_exception(original);
    } catch (const std::exception& e) {
      message += ": ";
      message += e.what();
    } catch (...) {
      message += ": non-std::exception payload";
    }
  }
  return message;
}

}  // namespace detail

// An exception escaped a user callback, or a callback returned an illegal
// result (e.g., a thread function returning BlockLabel). The thread id is the
// model::ThreadId of the thread whose callback misbehaved, when known.
class user_code_error : public error {
 public:
  user_code_error(const UserCallbackKind kind, const std::optional<std::uint32_t> thread,
                  std::exception_ptr original, const std::string& detail = {})
      : error(detail::compose_user_code_message(kind, thread, original, detail)),
        kind_(kind),
        thread_(thread),
        original_(std::move(original)) {}

  [[nodiscard]] UserCallbackKind kind() const noexcept { return kind_; }

  [[nodiscard]] std::optional<std::uint32_t> thread() const noexcept { return thread_; }

  [[nodiscard]] bool has_original() const noexcept { return static_cast<bool>(original_); }

  [[nodiscard]] std::exception_ptr original() const noexcept { return original_; }

  // Rethrows the exception that escaped the user callback. Illegal-result
  // errors (e.g., a returned BlockLabel) have no original exception.
  [[noreturn]] void rethrow_original() const {
    if (!original_) {
      throw precondition_error("user_code_error carries no original exception");
    }
    std::rethrow_exception(original_);
  }

 private:
  UserCallbackKind kind_;
  std::optional<std::uint32_t> thread_;
  std::exception_ptr original_;
};

}  // namespace dpor
