#pragma once

// Two-Phase Commit (2PC) protocol implementation.
//
// Real, runnable 2PC logic with no model-checking awareness.
// Coordinator and Participants communicate over an abstract Network interface,
// which can be backed by real UDP or a DPOR simulation adapter.

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
    iss >> from >> vote_str;
    return VoteMsg{from, vote_str == "YES" ? Vote::Yes : Vote::No};
  }
  if (tag == "DECISION") {
    std::string dec_str;
    iss >> dec_str;
    return DecisionMsg{dec_str == "COMMIT" ? Decision::Commit
                                           : Decision::Abort};
  }
  if (tag == "ACK") {
    std::size_t from{};
    iss >> from;
    return Ack{from};
  }
  throw std::invalid_argument("unknown message tag: " + tag);
}

// ---------------------------------------------------------------------------
// Network interface
// ---------------------------------------------------------------------------

class Network {
 public:
  virtual ~Network() = default;

  // Send a message to a node. destination is a ParticipantId
  // (kCoordinator for coordinator, 1..N for participants).
  virtual void send(ParticipantId destination, const Message& msg) = 0;

  // Blocking receive: returns the next incoming message.
  virtual Message receive() = 0;
};

// ---------------------------------------------------------------------------
// Coordinator
// ---------------------------------------------------------------------------

class Coordinator {
 public:
  explicit Coordinator(std::size_t num_participants,
                       bool crash_between_phases = false)
      : num_participants_(num_participants),
        crash_between_phases_(crash_between_phases) {}

  void run(Network& net) {
    // Phase 1: send Prepare to all participants.
    for (std::size_t pid = 1; pid <= num_participants_; ++pid) {
      net.send(pid, Prepare{});
    }

    // Phase 1: collect votes.
    bool all_yes = true;
    for (std::size_t i = 0; i < num_participants_; ++i) {
      auto msg = net.receive();
      const auto* vote = std::get_if<VoteMsg>(&msg);
      if (vote == nullptr || vote->vote == Vote::No) {
        all_yes = false;
      }
    }

    decision_ = all_yes ? Decision::Commit : Decision::Abort;

    // Simulate crash: skip phase 2 entirely.
    if (crash_between_phases_) {
      return;
    }

    // Phase 2: send decision to all participants.
    for (std::size_t pid = 1; pid <= num_participants_; ++pid) {
      net.send(pid, DecisionMsg{*decision_});
    }

    // Phase 2: collect acks.
    for (std::size_t i = 0; i < num_participants_; ++i) {
      net.receive();
    }
  }

  [[nodiscard]] std::optional<Decision> decision() const noexcept {
    return decision_;
  }

 private:
  std::size_t num_participants_;
  bool crash_between_phases_;
  std::optional<Decision> decision_;
};

// ---------------------------------------------------------------------------
// Participant
// ---------------------------------------------------------------------------

class Participant {
 public:
  explicit Participant(ParticipantId id, Vote vote) : id_(id), vote_(vote) {}

  void run(Network& net) {
    // Wait for Prepare.
    net.receive();

    // Send vote.
    net.send(kCoordinator, VoteMsg{id_, vote_});

    // Wait for decision.
    auto msg = net.receive();
    const auto* dec = std::get_if<DecisionMsg>(&msg);
    if (dec != nullptr) {
      outcome_ = dec->decision;
    }

    // Send ack.
    net.send(kCoordinator, Ack{id_});
  }

  [[nodiscard]] ParticipantId id() const noexcept { return id_; }
  [[nodiscard]] std::optional<Decision> outcome() const noexcept {
    return outcome_;
  }

 private:
  ParticipantId id_;
  Vote vote_;
  std::optional<Decision> outcome_;
};

}  // namespace tpc
