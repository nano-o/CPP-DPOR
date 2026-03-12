#pragma once

#include "dpor/algo/program.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include <compare>
#include <cstdint>
#include <functional>

namespace tpc_sim {

struct SimValue {
  enum class Tag : std::uint8_t {
    Invalid = 0,
    PrepareMessage,
    VoteYesMessage,
    VoteNoMessage,
    DecisionCommitMessage,
    DecisionAbortMessage,
    AckMessage,
    VoteChoiceYes,
    VoteChoiceNo,
    CrashChoiceNoCrash,
    CrashChoiceCrash,
  };

  std::uint64_t encoded{0};

  [[nodiscard]] constexpr auto operator<=>(const SimValue&) const = default;
};

using EventLabel = dpor::model::EventLabelT<SimValue>;
using SendLabel = dpor::model::SendLabelT<SimValue>;
using ReceiveLabel = dpor::model::ReceiveLabelT<SimValue>;
using NondeterministicChoiceLabel =
    dpor::model::NondeterministicChoiceLabelT<SimValue>;
using ObservedValue = dpor::model::ObservedValueT<SimValue>;
using ExplorationGraph = dpor::model::ExplorationGraphT<SimValue>;
using ThreadTrace = dpor::algo::ThreadTraceT<SimValue>;
using ThreadFunction = dpor::algo::ThreadFunctionT<SimValue>;
using Program = dpor::algo::ProgramT<SimValue>;

}  // namespace tpc_sim

namespace std {

template <>
struct hash<tpc_sim::SimValue> {
  [[nodiscard]] std::size_t operator()(tpc_sim::SimValue value) const noexcept {
    return hash<std::uint64_t>{}(value.encoded);
  }
};

}  // namespace std
