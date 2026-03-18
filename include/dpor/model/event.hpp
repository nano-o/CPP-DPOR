#pragma once

// Event layer design rationale:
// - Mirror Must-style execution-graph entities (send/receive/nondet/block/error)
//   while remaining practical for existing codebases.
// - Keep payload type generic (`ValueT`) so integrations can use native message
//   structures from the system under test.
// - Keep thread/index identifiers simple and concrete for now; they are internal
//   bookkeeping fields that benefit from stable, lightweight types.
// - Model receive compatibility via a predicate (`matches`) instead of only
//   explicit value sets, enabling post-hoc adapters that call existing logic.
// - Provide default aliases (`Event`, `SendLabel`, ...) so common usage stays
//   concise when `std::string` payloads are sufficient.

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dpor::model {

// Active thread IDs are used as direct indices into several internal vectors.
// Programs are runtime-validated to use a compact contiguous 0-based or 1-based
// range before exploration begins.
using ThreadId = std::uint32_t;
using EventIndex = std::uint32_t;
using Value = std::string;

enum class ReceiveMode : std::uint8_t { Blocking, NonBlocking };

struct BottomValue {
  bool operator==(const BottomValue&) const = default;
};

template <typename ValueT>
struct ObservedValueT {
  std::variant<ValueT, BottomValue> data;

  ObservedValueT() : data(BottomValue{}) {}

  ObservedValueT(ValueT value) : data(std::move(value)) {}

  ObservedValueT(BottomValue bottom) : data(bottom) {}

  [[nodiscard]] bool is_bottom() const noexcept {
    return std::holds_alternative<BottomValue>(data);
  }

  [[nodiscard]] const ValueT* as_value() const noexcept { return std::get_if<ValueT>(&data); }

  [[nodiscard]] const ValueT& value() const {
    const auto* observed = as_value();
    if (observed == nullptr) {
      throw std::logic_error("observed value is bottom");
    }
    return *observed;
  }

  [[nodiscard]] static ObservedValueT bottom() { return ObservedValueT{BottomValue{}}; }

  [[nodiscard]] bool operator==(const ObservedValueT&) const = default;

  [[nodiscard]] bool operator==(const ValueT& other) const {
    const auto* observed = as_value();
    return observed != nullptr && *observed == other;
  }
};

template <typename ValueT>
[[nodiscard]] inline bool operator==(const ValueT& lhs, const ObservedValueT<ValueT>& rhs) {
  return rhs == lhs;
}

using ObservedValue = ObservedValueT<Value>;

// Receive matcher predicate. Must be deterministic and side-effect-free: the
// same ValueT input must always produce the same bool result. DPOR soundness
// and completeness depend on this invariant. Stateful or mutable captures will
// silently invalidate exploration guarantees.
//
// Additionally, `std::function` keeps matcher semantics flexible but makes
// matcher identity opaque. As a result, `ReceiveLabelT` equality is not
// structurally meaningful. This matters for future memoization/reduction
// features (e.g., sleep sets), where stable matcher identity may be required.
template <typename ValueT>
using ReceiveMatchFnT = std::function<bool(const ValueT&)>;

template <typename ValueT>
struct ReceiveLabelT {
  ReceiveMode mode{ReceiveMode::Blocking};
  // Predicate deciding whether this receive may consume a candidate payload.
  ReceiveMatchFnT<ValueT> matches{[](const ValueT&) { return true; }};

  [[nodiscard]] bool is_blocking() const noexcept { return mode == ReceiveMode::Blocking; }

  [[nodiscard]] bool is_nonblocking() const noexcept { return mode == ReceiveMode::NonBlocking; }

  [[nodiscard]] bool accepts(const ValueT& value) const { return matches(value); }
};

template <typename ValueT>
struct SendLabelT {
  ThreadId destination{};
  ValueT value;
};

// Nondeterministic choice label. When `choices` is non-empty, the DPOR revisit
// condition uses min_element and equality comparison on ValueT, so ValueT must
// provide operator< and operator== in that case.
template <typename ValueT>
struct NondeterministicChoiceLabelT {
  ValueT value;
  std::vector<ValueT> choices{};  // The set S of possible values for this choice.
};

struct BlockLabel {};

struct ErrorLabel {
  std::string message;
};

template <typename ValueT>
using EventLabelT = std::variant<ReceiveLabelT<ValueT>, SendLabelT<ValueT>,
                                 NondeterministicChoiceLabelT<ValueT>, BlockLabel, ErrorLabel>;

template <typename ValueT>
struct EventT {
  ThreadId thread{};
  // Typically assigned by ExecutionGraph insertion APIs, not by callers.
  EventIndex index{};
  EventLabelT<ValueT> label{};
};

using ReceiveLabel = ReceiveLabelT<Value>;
using SendLabel = SendLabelT<Value>;
using NondeterministicChoiceLabel = NondeterministicChoiceLabelT<Value>;
using EventLabel = EventLabelT<Value>;
using Event = EventT<Value>;
using ReceiveMatchFn = ReceiveMatchFnT<Value>;

template <typename ValueT>
[[nodiscard]] inline ReceiveMatchFnT<ValueT> match_any_value() {
  return [](const ValueT&) { return true; };
}

template <typename ValueT>
[[nodiscard]] inline ReceiveLabelT<ValueT> make_receive_label(
    ReceiveMatchFnT<ValueT> matcher = match_any_value<ValueT>(),
    ReceiveMode mode = ReceiveMode::Blocking) {
  return ReceiveLabelT<ValueT>{
      .mode = mode,
      .matches = std::move(matcher),
  };
}

template <typename ValueT>
[[nodiscard]] inline ReceiveLabelT<ValueT> make_nonblocking_receive_label(
    ReceiveMatchFnT<ValueT> matcher = match_any_value<ValueT>()) {
  return make_receive_label<ValueT>(std::move(matcher), ReceiveMode::NonBlocking);
}

template <typename ValueT>
  requires std::equality_comparable<ValueT>
[[nodiscard]] inline ReceiveLabelT<ValueT> make_receive_label_from_values(
    std::vector<ValueT> accepted_values, ReceiveMode mode = ReceiveMode::Blocking) {
  return make_receive_label<ValueT>(
      [accepted_values = std::move(accepted_values)](const ValueT& candidate) {
        return std::find(accepted_values.begin(), accepted_values.end(), candidate) !=
               accepted_values.end();
      },
      mode);
}

template <typename ValueT>
[[nodiscard]] inline bool is_receive(const EventT<ValueT>& event) noexcept {
  return std::holds_alternative<ReceiveLabelT<ValueT>>(event.label);
}

template <typename ValueT>
[[nodiscard]] inline bool is_send(const EventT<ValueT>& event) noexcept {
  return std::holds_alternative<SendLabelT<ValueT>>(event.label);
}

template <typename ValueT>
[[nodiscard]] inline bool is_nondeterministic_choice(const EventT<ValueT>& event) noexcept {
  return std::holds_alternative<NondeterministicChoiceLabelT<ValueT>>(event.label);
}

template <typename ValueT>
[[nodiscard]] inline bool is_block(const EventT<ValueT>& event) noexcept {
  return std::holds_alternative<BlockLabel>(event.label);
}

template <typename ValueT>
[[nodiscard]] inline bool is_error(const EventT<ValueT>& event) noexcept {
  return std::holds_alternative<ErrorLabel>(event.label);
}

template <typename ValueT>
[[nodiscard]] inline const ReceiveLabelT<ValueT>* as_receive(const EventT<ValueT>& event) noexcept {
  return std::get_if<ReceiveLabelT<ValueT>>(&event.label);
}

template <typename ValueT>
[[nodiscard]] inline const SendLabelT<ValueT>* as_send(const EventT<ValueT>& event) noexcept {
  return std::get_if<SendLabelT<ValueT>>(&event.label);
}

template <typename ValueT>
[[nodiscard]] inline const NondeterministicChoiceLabelT<ValueT>* as_nondeterministic_choice(
    const EventT<ValueT>& event) noexcept {
  return std::get_if<NondeterministicChoiceLabelT<ValueT>>(&event.label);
}

template <typename ValueT>
[[nodiscard]] inline const BlockLabel* as_block(const EventT<ValueT>& event) noexcept {
  return std::get_if<BlockLabel>(&event.label);
}

template <typename ValueT>
[[nodiscard]] inline const ErrorLabel* as_error(const EventT<ValueT>& event) noexcept {
  return std::get_if<ErrorLabel>(&event.label);
}

}  // namespace dpor::model
