#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dpor::model {

using ThreadId = std::uint32_t;
using EventIndex = std::uint32_t;
using Value = std::string;

enum class ReceiveMode {
  Blocking,
  NonBlocking,
};

template <typename ValueT>
using ReceiveMatchFnT = std::function<bool(const ValueT&)>;

template <typename ValueT>
struct ReceiveLabelT {
  ReceiveMode mode{ReceiveMode::Blocking};
  ReceiveMatchFnT<ValueT> matches{[](const ValueT&) { return true; }};

  [[nodiscard]] bool accepts(const ValueT& value) const {
    return matches(value);
  }
};

template <typename ValueT>
struct SendLabelT {
  ThreadId destination{};
  ValueT value;
};

template <typename ValueT>
struct NondeterministicChoiceLabelT {
  ValueT value;
};

struct BlockLabel {};

struct ErrorLabel {};

template <typename ValueT>
using EventLabelT = std::variant<
    ReceiveLabelT<ValueT>,
    SendLabelT<ValueT>,
    NondeterministicChoiceLabelT<ValueT>,
    BlockLabel,
    ErrorLabel>;

template <typename ValueT>
struct EventT {
  ThreadId thread{};
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
    ReceiveMode mode,
    ReceiveMatchFnT<ValueT> matcher = match_any_value<ValueT>()) {
  return ReceiveLabelT<ValueT>{
      .mode = mode,
      .matches = std::move(matcher),
  };
}

template <typename ValueT>
  requires std::equality_comparable<ValueT>
[[nodiscard]] inline ReceiveLabelT<ValueT> make_receive_label_from_values(
    ReceiveMode mode,
    std::vector<ValueT> accepted_values) {
  return make_receive_label<ValueT>(
      mode,
      [accepted_values = std::move(accepted_values)](const ValueT& candidate) {
        return std::find(accepted_values.begin(), accepted_values.end(), candidate) !=
               accepted_values.end();
      });
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
