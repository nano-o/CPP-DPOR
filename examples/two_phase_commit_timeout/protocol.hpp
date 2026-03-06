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
#include <functional>
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

class Environment;

using TimerId = std::size_t;
using TimerCallback = std::function<void(Environment&)>;

class Environment {
 public:
  virtual ~Environment() = default;

  // Send a message to a node. destination is a ParticipantId
  // (kCoordinator for coordinator, 1..N for participants).
  virtual void send(ParticipantId destination, const Message& msg) = 0;

  // Query the environment for this participant's vote.
  virtual Vote get_vote() = 0;

  // Start a timer. When timeout_ms elapses without the timer being cancelled,
  // the callback is invoked. If a timer with the same id already exists, it is
  // replaced.
  virtual void set_timer(TimerId id, std::size_t timeout_ms,
                         TimerCallback callback) = 0;

  // Cancel a previously set timer. No-op if the timer doesn't exist or has
  // already fired.
  virtual void cancel_timer(TimerId id) = 0;
};

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

class Coordinator {
 public:
  explicit Coordinator(std::size_t num_participants,
                       bool bug_on_p1_no = false,
                       std::size_t vote_timeout_ms = 100,
                       std::size_t ack_timeout_ms = 1000)
      : num_participants_(num_participants),
        bug_on_p1_no_(bug_on_p1_no),
        vote_timeout_ms_(vote_timeout_ms),
        ack_timeout_ms_(ack_timeout_ms) {}

  // Kick off the protocol: sends Prepare to all participants.
  // Returns true if the protocol needs incoming messages, false if done.
  bool start(Environment& env) {
    phase_ = Phase::CollectingVotes;
    all_yes_ = true;
    votes_received_ = 0;
    acks_received_ = 0;
    decision_ = std::nullopt;
    vote_seen_.assign(num_participants_ + 1, false);
    ack_seen_.assign(num_participants_ + 1, false);

    for (std::size_t pid = 1; pid <= num_participants_; ++pid) {
      env.send(pid, Prepare{});
    }
    env.set_timer(kVoteTimeoutTimerId, vote_timeout_ms_,
                  [this](Environment& timer_env) {
                    on_vote_timeout(timer_env);
                  });
    return true;
  }

  // Process one incoming message.
  // Returns true if the protocol needs more messages, false if done.
  bool receive(Environment& env, const Message& msg) {
    switch (phase_) {
      case Phase::CollectingVotes: {
        const auto* vote = std::get_if<VoteMsg>(&msg);
        if (vote == nullptr || !is_valid_participant(vote->from) ||
            vote_seen_[vote->from]) {
          return true;
        }

        vote_seen_[vote->from] = true;
        ++votes_received_;
        if (vote->vote == Vote::No) {
          all_yes_ = false;
          if (bug_on_p1_no_ && vote->from == 1) {
            throw std::logic_error(
                "BUG: coordinator cannot handle No vote from participant 1");
          }
        }
        if (votes_received_ < num_participants_) {
          return true;
        }
        env.cancel_timer(kVoteTimeoutTimerId);
        // All votes collected — send decision.
        decision_ = all_yes_ ? Decision::Commit : Decision::Abort;
        broadcast_decision(env);
        return true;
      }
      case Phase::CollectingAcks: {
        const auto* ack = std::get_if<Ack>(&msg);
        if (ack == nullptr || !is_valid_participant(ack->from) ||
            ack_seen_[ack->from]) {
          return true;
        }

        ack_seen_[ack->from] = true;
        ++acks_received_;
        if (acks_received_ < num_participants_) {
          return true;
        }
        env.cancel_timer(kAckTimeoutTimerId);
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
  static constexpr TimerId kVoteTimeoutTimerId = 1;
  static constexpr TimerId kAckTimeoutTimerId = 2;

  bool is_valid_participant(ParticipantId id) const noexcept {
    return id >= 1 && id <= num_participants_;
  }

  void on_vote_timeout(Environment& env) {
    if (phase_ != Phase::CollectingVotes) {
      return;
    }
    all_yes_ = false;
    decision_ = Decision::Abort;
    broadcast_decision(env);
  }

  void on_ack_timeout(Environment& env) {
    if (phase_ != Phase::CollectingAcks) {
      return;
    }
    phase_ = Phase::Done;
    // Wake blocking runtimes so they can observe completion and return.
    env.send(kCoordinator, Ack{0});
  }

  void broadcast_decision(Environment& env) {
    for (std::size_t pid = 1; pid <= num_participants_; ++pid) {
      env.send(pid, DecisionMsg{*decision_});
    }
    env.set_timer(kAckTimeoutTimerId, ack_timeout_ms_,
                  [this](Environment& timer_env) {
                    on_ack_timeout(timer_env);
                  });
    phase_ = Phase::CollectingAcks;
  }

  enum class Phase { CollectingVotes, CollectingAcks, Done };

  std::size_t num_participants_;
  bool bug_on_p1_no_;
  std::size_t vote_timeout_ms_;
  std::size_t ack_timeout_ms_;
  std::optional<Decision> decision_;

  Phase phase_ = Phase::CollectingVotes;
  bool all_yes_ = true;
  std::size_t votes_received_ = 0;
  std::size_t acks_received_ = 0;
  std::vector<bool> vote_seen_;
  std::vector<bool> ack_seen_;
};

// ---------------------------------------------------------------------------
// Participant
// ---------------------------------------------------------------------------

class Participant {
 public:
  explicit Participant(ParticipantId id,
                       std::size_t decision_timeout_ms = 1000)
      : id_(id), decision_timeout_ms_(decision_timeout_ms) {}

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
        if (!std::holds_alternative<Prepare>(msg)) {
          return true;
        }
        // Got Prepare — vote and send it.
        auto vote = env.get_vote();
        env.send(kCoordinator, VoteMsg{id_, vote});
        env.set_timer(kDecisionTimeoutTimerId, decision_timeout_ms_,
                      [this](Environment& timer_env) {
                        on_decision_timeout(timer_env);
                      });
        state_ = State::WaitDecision;
        return true;
      }
      case State::WaitDecision: {
        const auto* dec = std::get_if<DecisionMsg>(&msg);
        if (dec == nullptr) {
          return true;
        }

        env.cancel_timer(kDecisionTimeoutTimerId);
        outcome_ = dec->decision;
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
  static constexpr TimerId kDecisionTimeoutTimerId = 1;

  void on_decision_timeout(Environment& env) {
    if (state_ != State::WaitDecision) {
      return;
    }

    outcome_ = Decision::Abort;
    state_ = State::Done;

    // Wake blocking runtimes so they can observe completion and return.
    env.send(id_, DecisionMsg{Decision::Abort});
  }

  enum class State { WaitPrepare, WaitDecision, Done };

  ParticipantId id_;
  std::size_t decision_timeout_ms_;
  State state_ = State::WaitPrepare;
  std::optional<Decision> outcome_;
};

}  // namespace tpc
