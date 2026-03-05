#pragma once

// Two-Phase Commit (2PC) protocol implementation.
//
// Real, runnable 2PC logic with no model-checking awareness.
// Coordinator and Participants communicate over an abstract Environment
// interface, which can be backed by real UDP or a DPOR simulation adapter.
//
// The protocol is driven by the environment: the environment calls start()
// once, then feeds incoming messages via receive() until the protocol is done.

#include <cstddef>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace tpc {

// ---------------------------------------------------------------------------
// Addressing
// ---------------------------------------------------------------------------

using ParticipantId = std::size_t;
constexpr ParticipantId kCoordinator = 0;

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------

struct Prepare {};

enum class Vote { Yes, No };

struct VoteMsg {
  ParticipantId from;
  Vote vote;
};

enum class Decision { Commit, Abort };

struct DecisionMsg {
  Decision decision;
};

struct Ack {
  ParticipantId from;
};

using Message = std::variant<Prepare, VoteMsg, DecisionMsg, Ack>;

// ---------------------------------------------------------------------------
// Serialization (simple text format for network transport)
// ---------------------------------------------------------------------------

inline std::string serialize(const Message& msg) {
  return std::visit(
      [](const auto& m) -> std::string {
        using T = std::decay_t<decltype(m)>;
        if constexpr (std::is_same_v<T, Prepare>) {
          return "PREPARE";
        } else if constexpr (std::is_same_v<T, VoteMsg>) {
          return std::string("VOTE ") + std::to_string(m.from) + " " +
                 (m.vote == Vote::Yes ? "YES" : "NO");
        } else if constexpr (std::is_same_v<T, DecisionMsg>) {
          return std::string("DECISION ") +
                 (m.decision == Decision::Commit ? "COMMIT" : "ABORT");
        } else if constexpr (std::is_same_v<T, Ack>) {
          return std::string("ACK ") + std::to_string(m.from);
        }
      },
      msg);
}

inline Message deserialize(const std::string& data) {
  std::istringstream iss(data);
  std::string tag;
  iss >> tag;

  if (tag == "PREPARE") {
    return Prepare{};
  }
  if (tag == "VOTE") {
    std::size_t from{};
    std::string vote_str;
    if (!(iss >> from >> vote_str)) {
      throw std::invalid_argument("malformed VOTE message: " + data);
    }
    if (vote_str == "YES") {
      return VoteMsg{from, Vote::Yes};
    }
    if (vote_str == "NO") {
      return VoteMsg{from, Vote::No};
    }
    throw std::invalid_argument("unknown vote value: " + vote_str);
  }
  if (tag == "DECISION") {
    std::string dec_str;
    iss >> dec_str;
    if (dec_str == "COMMIT") {
      return DecisionMsg{Decision::Commit};
    }
    if (dec_str == "ABORT") {
      return DecisionMsg{Decision::Abort};
    }
    throw std::invalid_argument("unknown decision value: " + dec_str);
  }
  if (tag == "ACK") {
    std::size_t from{};
    if (!(iss >> from)) {
      throw std::invalid_argument("malformed ACK message: " + data);
    }
    return Ack{from};
  }
  throw std::invalid_argument("unknown message tag: " + tag);
}

// ---------------------------------------------------------------------------
// Environment interface
// ---------------------------------------------------------------------------

class Environment {
 public:
  virtual ~Environment() = default;

  // Send a message to a node. destination is a ParticipantId
  // (kCoordinator for coordinator, 1..N for participants).
  virtual void send(ParticipantId destination, const Message& msg) = 0;

  // Query the environment for this participant's vote.
  virtual Vote get_vote() = 0;
};

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

class Coordinator {
 public:
  explicit Coordinator(std::size_t num_participants,
                       bool bug_on_p1_no = false)
      : num_participants_(num_participants), bug_on_p1_no_(bug_on_p1_no) {}

  // Kick off the protocol: sends Prepare to all participants.
  // Returns true if the protocol needs incoming messages, false if done.
  bool start(Environment& env) {
    phase_ = Phase::CollectingVotes;
    all_yes_ = true;
    votes_received_ = 0;
    acks_received_ = 0;
    decision_ = std::nullopt;

    for (std::size_t pid = 1; pid <= num_participants_; ++pid) {
      env.send(pid, Prepare{});
    }
    return true;
  }

  // Process one incoming message.
  // Returns true if the protocol needs more messages, false if done.
  bool receive(Environment& env, const Message& msg) {
    switch (phase_) {
      case Phase::CollectingVotes: {
        const auto* vote = std::get_if<VoteMsg>(&msg);
        if (vote == nullptr || vote->vote == Vote::No) {
          all_yes_ = false;
          if (bug_on_p1_no_ && vote != nullptr && vote->from == 1) {
            throw std::logic_error(
                "BUG: coordinator cannot handle No vote from participant 1");
          }
        }
        ++votes_received_;
        if (votes_received_ < num_participants_) {
          return true;
        }
        // All votes collected — send decision.
        decision_ = all_yes_ ? Decision::Commit : Decision::Abort;
        for (std::size_t pid = 1; pid <= num_participants_; ++pid) {
          env.send(pid, DecisionMsg{*decision_});
        }
        phase_ = Phase::CollectingAcks;
        return true;
      }
      case Phase::CollectingAcks: {
        ++acks_received_;
        if (acks_received_ < num_participants_) {
          return true;
        }
        phase_ = Phase::Done;
        return false;
      }
      case Phase::Done:
        return false;
    }
    return false;
  }

  [[nodiscard]] std::optional<Decision> decision() const noexcept {
    return decision_;
  }

 private:
  enum class Phase { CollectingVotes, CollectingAcks, Done };

  std::size_t num_participants_;
  bool bug_on_p1_no_;
  std::optional<Decision> decision_;

  Phase phase_ = Phase::CollectingVotes;
  bool all_yes_ = true;
  std::size_t votes_received_ = 0;
  std::size_t acks_received_ = 0;
};

// ---------------------------------------------------------------------------
// Participant
// ---------------------------------------------------------------------------

class Participant {
 public:
  explicit Participant(ParticipantId id) : id_(id) {}

  // Kick off the protocol: participant waits for Prepare, so it just
  // signals that it needs a message.
  // Returns true if the protocol needs incoming messages, false if done.
  bool start(Environment& /*env*/) {
    state_ = State::WaitPrepare;
    outcome_ = std::nullopt;
    return true;
  }

  // Process one incoming message.
  // Returns true if the protocol needs more messages, false if done.
  bool receive(Environment& env, const Message& msg) {
    switch (state_) {
      case State::WaitPrepare: {
        // Got Prepare — vote and send it.
        auto vote = env.get_vote();
        env.send(kCoordinator, VoteMsg{id_, vote});
        state_ = State::WaitDecision;
        return true;
      }
      case State::WaitDecision: {
        const auto* dec = std::get_if<DecisionMsg>(&msg);
        if (dec != nullptr) {
          outcome_ = dec->decision;
        }
        env.send(kCoordinator, Ack{id_});
        state_ = State::Done;
        return false;
      }
      case State::Done:
        return false;
    }
    return false;
  }

  [[nodiscard]] ParticipantId id() const noexcept { return id_; }
  [[nodiscard]] std::optional<Decision> outcome() const noexcept {
    return outcome_;
  }

 private:
  enum class State { WaitPrepare, WaitDecision, Done };

  ParticipantId id_;
  State state_ = State::WaitPrepare;
  std::optional<Decision> outcome_;
};

}  // namespace tpc
