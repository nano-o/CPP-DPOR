#pragma once

#include "../protocol.hpp"
#include "dpor_types.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <variant>

namespace tpc_sim {

namespace sim_value_encoding {

constexpr std::uint64_t kTagMask = 0xff;
constexpr std::uint64_t kPayloadShift = 8;

[[nodiscard]] constexpr SimValue make_value(SimValue::Tag tag, std::uint64_t payload = 0) {
  return SimValue{
      .encoded = (payload << kPayloadShift) | static_cast<std::uint64_t>(tag),
  };
}

[[nodiscard]] constexpr SimValue::Tag tag_of(SimValue value) {
  return static_cast<SimValue::Tag>(value.encoded & kTagMask);
}

[[nodiscard]] constexpr tpc::ParticipantId payload_of(SimValue value) {
  return static_cast<tpc::ParticipantId>(value.encoded >> kPayloadShift);
}

}  // namespace sim_value_encoding

[[nodiscard]] constexpr SimValue prepare_message() {
  return sim_value_encoding::make_value(SimValue::Tag::PrepareMessage);
}

[[nodiscard]] constexpr SimValue vote_message(tpc::ParticipantId from, tpc::Vote vote) {
  return sim_value_encoding::make_value(
      vote == tpc::Vote::Yes ? SimValue::Tag::VoteYesMessage : SimValue::Tag::VoteNoMessage,
      static_cast<std::uint64_t>(from));
}

[[nodiscard]] constexpr SimValue decision_message(tpc::Decision decision) {
  return sim_value_encoding::make_value(decision == tpc::Decision::Commit
                                            ? SimValue::Tag::DecisionCommitMessage
                                            : SimValue::Tag::DecisionAbortMessage);
}

[[nodiscard]] constexpr SimValue ack_message(tpc::ParticipantId from) {
  return sim_value_encoding::make_value(SimValue::Tag::AckMessage,
                                        static_cast<std::uint64_t>(from));
}

[[nodiscard]] inline SimValue encode_message(const tpc::Message& msg) {
  if (std::holds_alternative<tpc::Prepare>(msg)) {
    return prepare_message();
  }
  if (const auto* vote = std::get_if<tpc::VoteMsg>(&msg)) {
    return vote_message(vote->from, vote->vote);
  }
  if (const auto* decision = std::get_if<tpc::DecisionMsg>(&msg)) {
    return decision_message(decision->decision);
  }
  const auto* ack = std::get_if<tpc::Ack>(&msg);
  if (ack == nullptr) {
    throw std::logic_error("unsupported 2PC message kind");
  }
  return ack_message(ack->from);
}

[[nodiscard]] inline tpc::Message decode_message(SimValue value) {
  switch (sim_value_encoding::tag_of(value)) {
    case SimValue::Tag::PrepareMessage:
      return tpc::Prepare{};
    case SimValue::Tag::VoteYesMessage:
      return tpc::VoteMsg{sim_value_encoding::payload_of(value), tpc::Vote::Yes};
    case SimValue::Tag::VoteNoMessage:
      return tpc::VoteMsg{sim_value_encoding::payload_of(value), tpc::Vote::No};
    case SimValue::Tag::DecisionCommitMessage:
      return tpc::DecisionMsg{tpc::Decision::Commit};
    case SimValue::Tag::DecisionAbortMessage:
      return tpc::DecisionMsg{tpc::Decision::Abort};
    case SimValue::Tag::AckMessage:
      return tpc::Ack{sim_value_encoding::payload_of(value)};
    default:
      throw std::logic_error("observed value is not a message");
  }
}

[[nodiscard]] constexpr SimValue vote_choice(tpc::Vote vote) {
  return sim_value_encoding::make_value(vote == tpc::Vote::Yes ? SimValue::Tag::VoteChoiceYes
                                                               : SimValue::Tag::VoteChoiceNo);
}

[[nodiscard]] inline tpc::Vote decode_vote_choice(SimValue value) {
  switch (sim_value_encoding::tag_of(value)) {
    case SimValue::Tag::VoteChoiceYes:
      return tpc::Vote::Yes;
    case SimValue::Tag::VoteChoiceNo:
      return tpc::Vote::No;
    default:
      throw std::logic_error("observed value is not a vote choice");
  }
}

[[nodiscard]] constexpr SimValue crash_choice(bool crash) {
  return sim_value_encoding::make_value(crash ? SimValue::Tag::CrashChoiceCrash
                                              : SimValue::Tag::CrashChoiceNoCrash);
}

[[nodiscard]] inline bool decode_crash_choice(SimValue value) {
  switch (sim_value_encoding::tag_of(value)) {
    case SimValue::Tag::CrashChoiceNoCrash:
      return false;
    case SimValue::Tag::CrashChoiceCrash:
      return true;
    default:
      throw std::logic_error("observed value is not a crash choice");
  }
}

[[nodiscard]] inline std::optional<tpc::Decision> decode_decision_message(SimValue value) {
  switch (sim_value_encoding::tag_of(value)) {
    case SimValue::Tag::DecisionCommitMessage:
      return tpc::Decision::Commit;
    case SimValue::Tag::DecisionAbortMessage:
      return tpc::Decision::Abort;
    default:
      return std::nullopt;
  }
}

inline std::ostream& operator<<(std::ostream& os, SimValue value) {
  switch (sim_value_encoding::tag_of(value)) {
    case SimValue::Tag::Invalid:
      return os << "<invalid>";
    case SimValue::Tag::PrepareMessage:
      return os << "PREPARE";
    case SimValue::Tag::VoteYesMessage:
      return os << "VOTE " << sim_value_encoding::payload_of(value) << " YES";
    case SimValue::Tag::VoteNoMessage:
      return os << "VOTE " << sim_value_encoding::payload_of(value) << " NO";
    case SimValue::Tag::DecisionCommitMessage:
      return os << "DECISION COMMIT";
    case SimValue::Tag::DecisionAbortMessage:
      return os << "DECISION ABORT";
    case SimValue::Tag::AckMessage:
      return os << "ACK " << sim_value_encoding::payload_of(value);
    case SimValue::Tag::VoteChoiceYes:
      return os << "YES";
    case SimValue::Tag::VoteChoiceNo:
      return os << "NO";
    case SimValue::Tag::CrashChoiceNoCrash:
      return os << "no_crash";
    case SimValue::Tag::CrashChoiceCrash:
      return os << "crash";
  }
  return os << "<unknown>";
}

// DPOR ThreadIds start at 1. We map: coordinator (pid 0) -> tid 1,
// participant pid N -> tid N+1.
inline dpor::model::ThreadId participant_to_thread(tpc::ParticipantId pid) {
  return static_cast<dpor::model::ThreadId>(pid + 1);
}

}  // namespace tpc_sim
