#pragma once

// DPOR simulation adapter for Two-Phase Commit.
//
// Bridges the real tpc::Environment interface to the DPOR engine by
// intercepting send/get_vote calls and injecting crash nondeterminism.
//
// Core idea: each ThreadFunction call creates a fresh protocol object and
// SimEnvironment, drives it via start()/receive() (the event-driven protocol
// interface), fast-forwards through past I/O, and captures the current I/O
// as a DPOR EventLabel.

#include "protocol.hpp"

#include "dpor/algo/program.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

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

namespace sim_value_encoding {

constexpr std::uint64_t kTagMask = 0xff;
constexpr std::uint64_t kPayloadShift = 8;

[[nodiscard]] constexpr SimValue make_value(
    SimValue::Tag tag,
    std::uint64_t payload = 0) {
  return SimValue{
      .encoded = (payload << kPayloadShift) |
                 static_cast<std::uint64_t>(tag),
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

[[nodiscard]] constexpr SimValue vote_message(
    tpc::ParticipantId from,
    tpc::Vote vote) {
  return sim_value_encoding::make_value(
      vote == tpc::Vote::Yes ? SimValue::Tag::VoteYesMessage
                             : SimValue::Tag::VoteNoMessage,
      static_cast<std::uint64_t>(from));
}

[[nodiscard]] constexpr SimValue decision_message(tpc::Decision decision) {
  return sim_value_encoding::make_value(
      decision == tpc::Decision::Commit
          ? SimValue::Tag::DecisionCommitMessage
          : SimValue::Tag::DecisionAbortMessage);
}

[[nodiscard]] constexpr SimValue ack_message(tpc::ParticipantId from) {
  return sim_value_encoding::make_value(
      SimValue::Tag::AckMessage, static_cast<std::uint64_t>(from));
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
  return sim_value_encoding::make_value(
      vote == tpc::Vote::Yes ? SimValue::Tag::VoteChoiceYes
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
  return sim_value_encoding::make_value(
      crash ? SimValue::Tag::CrashChoiceCrash
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

[[nodiscard]] inline std::optional<tpc::Decision> decode_decision_message(
    SimValue value) {
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

// ---------------------------------------------------------------------------
// SimEnvironment: intercepting Environment implementation
// ---------------------------------------------------------------------------

// Exception used internally to terminate the protocol thread when a crash
// is injected during fast-forward.
struct CrashInjected {};
struct StepBoundaryReached {};
struct TimeoutSimulationFailure : std::logic_error {
  using std::logic_error::logic_error;
};

[[nodiscard]] inline EventLabel make_protocol_error_label(std::string message) {
  return EventLabel{dpor::model::ErrorLabel{.message = std::move(message)}};
}

template <typename ResultT, typename Fn>
[[nodiscard]] std::optional<EventLabel> invoke_protocol_step(
    ResultT& out,
    Fn&& fn) {
  try {
    out = std::forward<Fn>(fn)();
    return std::nullopt;
  } catch (const CrashInjected&) {
    throw;
  } catch (const StepBoundaryReached&) {
    throw;
  } catch (const TimeoutSimulationFailure&) {
    throw;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& ex) {
    return make_protocol_error_label(ex.what());
  } catch (...) {
    return make_protocol_error_label("uncaught non-standard exception");
  }
}

struct SimReceiveResult {
  enum class Kind {
    ReplayedMessage,
    ReplayedTimerFire,
    CapturedReceive,
  };

  Kind kind;
  std::optional<tpc::Message> message{};
  // Used only for ReplayedTimerFire: true means the timer callback kept the
  // protocol waiting for input, false means it completed the protocol.
  bool needs_message{true};

  [[nodiscard]] static SimReceiveResult replayed_message(tpc::Message msg) {
    return SimReceiveResult{
        .kind = Kind::ReplayedMessage,
        .message = std::move(msg),
    };
  }

  [[nodiscard]] static SimReceiveResult replayed_timer_fire(
      bool keep_waiting) {
    return SimReceiveResult{
        .kind = Kind::ReplayedTimerFire,
        .needs_message = keep_waiting,
    };
  }

  [[nodiscard]] static SimReceiveResult captured_receive() {
    return SimReceiveResult{
        .kind = Kind::CapturedReceive,
    };
  }
};

class SimEnvironment : public tpc::Environment {
 public:
  SimEnvironment(
      std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map,
      std::size_t target_io,
      const ThreadTrace& trace,
      std::size_t trace_offset,
      bool inject_crash = false)
      : id_map_(std::move(id_map)),
        target_io_(target_io),
        trace_(trace),
        trace_offset_(trace_offset),
        inject_crash_(inject_crash) {}

  void send(tpc::ParticipantId dest, const tpc::Message& msg) override {
    // Crash injection: when the coordinator is about to send a DecisionMsg
    // for the first time, inject a nondeterministic crash choice.
    if (inject_crash_ && !crash_injected_ &&
        std::holds_alternative<tpc::DecisionMsg>(msg)) {
      crash_injected_ = true;
      auto current = io_count_++;

      if (current < target_io_) {
        // Fast-forward: read crash choice from trace.
        auto idx = trace_offset_ + trace_consume_count_++;
        if (decode_sim_crash_choice(trace_value(idx))) {
          throw CrashInjected{};
        }
        // no_crash: fall through to process the actual send below.
      } else {
        // This is the target I/O: produce ND choice label.
        result_ = NondeterministicChoiceLabel{
            .value = {},
            .choices = {crash_choice(false), crash_choice(true)},
        };
        throw StepBoundaryReached{};
      }
    }

    auto current = io_count_++;

    if (current < target_io_) {
      // Fast-forward: past send, return immediately.
      return;
    }

    // This is the target I/O operation.
    result_ = SendLabel{
        .destination = id_map_(dest),
        .value = encode_sim_message(msg),
    };
    throw StepBoundaryReached{};
  }

  tpc::Vote get_vote() override {
    auto current = io_count_++;

    if (current < target_io_) {
      // Fast-forward: past vote choice, replay from trace.
      auto idx = trace_offset_ + trace_consume_count_++;
      return decode_sim_vote_choice(trace_value(idx));
    }

    // This is the target I/O operation: produce ND choice label.
    result_ = NondeterministicChoiceLabel{
        .value = {},
        .choices = {vote_choice(tpc::Vote::Yes), vote_choice(tpc::Vote::No)},
    };
    throw StepBoundaryReached{};
  }

  // Produce the next receive-step action for the current I/O, or replay it
  // from trace. When a timer is active, receives become non-blocking and
  // bottom in the trace means the timer fired.
  SimReceiveResult sim_receive() {
    auto current = io_count_++;

    if (current < target_io_) {
      // Fast-forward: past receive, replay either a message or a timeout.
      auto idx = trace_offset_ + trace_consume_count_++;
      const auto& observed = trace_entry(idx);
      if (observed.is_bottom()) {
        // Timer callbacks execute synchronously during replay. Any send/choice
        // they trigger flows through the normal interceptors below, so it is
        // either fast-forwarded (current < target_io_) or captured as the
        // target event (current == target_io_).
        return SimReceiveResult::replayed_timer_fire(fire_active_timer());
      }
      return SimReceiveResult::replayed_message(decode_sim_message(trace_value(idx)));
    }

    // This is the target I/O operation.
    if (has_active_timer()) {
      result_ = dpor::model::make_nonblocking_receive_label<SimValue>();
    } else {
      result_ = dpor::model::make_receive_label<SimValue>();
    }
    return SimReceiveResult::captured_receive();
  }

  void set_timer(tpc::TimerId id, std::size_t /*timeout_ms*/,
                 tpc::TimerCallback callback) override {
    // Matching UdpEnvironment, setting the same timer id refreshes/replaces it.
    // A different id would mean multiple simultaneously-active timers, which
    // this simplified adapter does not encode in bottom observations.
    // Treat this as a simulator limitation, not a protocol verification error:
    // the adapter cannot represent more than one concurrent timer in its
    // bottom-based receive encoding, so exploration must stop loudly here.
    if (active_timer_.has_value() && active_timer_->id != id) {
      throw TimeoutSimulationFailure(
          "SimEnvironment supports at most one active timer per thread");
    }
    active_timer_ = ActiveTimer{
        .id = id,
        .callback = std::move(callback),
    };
  }

  void cancel_timer(tpc::TimerId id) override {
    if (active_timer_.has_value() && active_timer_->id == id) {
      active_timer_.reset();
    }
  }

  [[nodiscard]] std::optional<EventLabel> result() const {
    return result_;
  }

 private:
  struct ActiveTimer {
    tpc::TimerId id;
    tpc::TimerCallback callback;
  };

  [[nodiscard]] bool has_active_timer() const noexcept {
    return active_timer_.has_value();
  }

  [[nodiscard]] const ThreadTrace::value_type& trace_entry(std::size_t idx) const {
    if (idx >= trace_.size()) {
      throw TimeoutSimulationFailure("trace shorter than expected for simulated replay");
    }
    return trace_[idx];
  }

  [[nodiscard]] const SimValue& trace_value(std::size_t idx) const {
    const auto& observed = trace_entry(idx);
    if (observed.is_bottom()) {
      throw TimeoutSimulationFailure("trace requested a concrete value but observed bottom");
    }
    return observed.value();
  }

  [[nodiscard]] SimValue encode_sim_message(const tpc::Message& msg) const {
    try {
      return encode_message(msg);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& ex) {
      throw TimeoutSimulationFailure(ex.what());
    }
  }

  [[nodiscard]] tpc::Vote decode_sim_vote_choice(SimValue value) const {
    try {
      return decode_vote_choice(value);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& ex) {
      throw TimeoutSimulationFailure(ex.what());
    }
  }

  [[nodiscard]] bool decode_sim_crash_choice(SimValue value) const {
    try {
      return decode_crash_choice(value);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& ex) {
      throw TimeoutSimulationFailure(ex.what());
    }
  }

  [[nodiscard]] tpc::Message decode_sim_message(SimValue value) const {
    try {
      return decode_message(value);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& ex) {
      throw TimeoutSimulationFailure(ex.what());
    }
  }

  [[nodiscard]] bool fire_active_timer() {
    if (!active_timer_.has_value()) {
      throw TimeoutSimulationFailure(
          "trace requested timer firing but no timer is active");
    }
    // Return the timer callback's continuation flag:
    // true => protocol is still waiting for input
    // false => protocol finished during the callback
    auto callback = std::move(active_timer_->callback);
    active_timer_.reset();
    bool keep_waiting = false;
    if (const auto error = invoke_protocol_step(keep_waiting, [&]() {
          return callback(*this);
        });
        error.has_value()) {
      // A protocol exception during timer replay makes the execution terminal
      // immediately. Reuse StepBoundaryReached to unwind back to
      // run_and_capture(), which returns this stored ErrorLabel as the next
      // DPOR event instead of attempting to continue replay after failure.
      result_ = *error;
      throw StepBoundaryReached{};
    }
    return keep_waiting;
  }

  std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map_;
  std::size_t target_io_;
  const ThreadTrace& trace_;
  std::size_t trace_offset_;
  bool inject_crash_;

  std::size_t io_count_{0};
  std::size_t trace_consume_count_{0};
  std::optional<EventLabel> result_;
  bool crash_injected_{false};
  std::optional<ActiveTimer> active_timer_;
};

// ---------------------------------------------------------------------------
// Thread ID mapping: ParticipantId -> DPOR ThreadId
// ---------------------------------------------------------------------------

// DPOR ThreadIds start at 1. We map: coordinator (pid 0) -> tid 1,
// participant pid N -> tid N+1.
inline dpor::model::ThreadId participant_to_thread(
    tpc::ParticipantId pid) {
  return static_cast<dpor::model::ThreadId>(pid + 1);
}

// ---------------------------------------------------------------------------
// ThreadFunction factories
// ---------------------------------------------------------------------------

// Drive a protocol object through start()/receive(), capturing a single I/O
// event via SimEnvironment.  Returns the captured EventLabel or nullopt if
// the protocol finished before reaching the target I/O.
template <typename ProtocolObj>
std::optional<EventLabel> run_and_capture(
    ProtocolObj& obj,
    SimEnvironment& env) {
  try {
    bool needs_message = false;
    if (const auto error = invoke_protocol_step(needs_message, [&]() {
          return obj.start(env);
        });
        error.has_value()) {
      return error;
    }
    while (needs_message) {
      auto receive_step = env.sim_receive();
      if (receive_step.kind == SimReceiveResult::Kind::CapturedReceive) {
        // Target I/O was a receive — label is stored in env.result().
        return env.result();
      }
      if (receive_step.kind == SimReceiveResult::Kind::ReplayedTimerFire) {
        needs_message = receive_step.needs_message;
        continue;
      }
      if (const auto error = invoke_protocol_step(needs_message, [&]() {
            return obj.receive(env, *receive_step.message);
          });
          error.has_value()) {
        return error;
      }
    }
    // Protocol finished before reaching target I/O.
    return std::nullopt;
  } catch (const CrashInjected&) {
    // Crash was injected during fast-forward -- protocol terminated.
    return std::nullopt;
  } catch (const StepBoundaryReached&) {
    // Captured one DPOR event from a send/get_vote call.
    return env.result();
  }
}

// Create a ThreadFunction for the coordinator.
inline ThreadFunction make_coordinator_function(
    std::size_t num_participants,
    bool inject_crash = true,
    bool bug_on_p1_no = false) {
  return [num_participants, inject_crash, bug_on_p1_no](
             const ThreadTrace& trace,
             std::size_t step) -> std::optional<EventLabel> {
    SimEnvironment env(participant_to_thread, step, trace, 0, inject_crash);
    std::optional<tpc::Coordinator> coord;
    try {
      coord.emplace(num_participants, bug_on_p1_no);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& ex) {
      return make_protocol_error_label(ex.what());
    } catch (...) {
      return make_protocol_error_label("uncaught non-standard exception");
    }
    return run_and_capture(*coord, env);
  };
}

// Create a ThreadFunction for a participant.
inline ThreadFunction make_participant_function(
    tpc::ParticipantId pid) {
  return [pid](const ThreadTrace& trace,
               std::size_t step) -> std::optional<EventLabel> {
    SimEnvironment env(participant_to_thread, step, trace, 0);
    std::optional<tpc::Participant> participant;
    try {
      participant.emplace(pid);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& ex) {
      return make_protocol_error_label(ex.what());
    } catch (...) {
      return make_protocol_error_label("uncaught non-standard exception");
    }
    return run_and_capture(*participant, env);
  };
}

// ---------------------------------------------------------------------------
// Program assembly
// ---------------------------------------------------------------------------

inline Program make_two_phase_commit_program(
    std::size_t num_participants,
    bool inject_crash = true,
    bool bug_on_p1_no = false) {
  Program prog;

  // Coordinator gets ThreadId 1 (= participant_to_thread(kCoordinator)).
  prog.threads[participant_to_thread(tpc::kCoordinator)] =
      make_coordinator_function(num_participants, inject_crash, bug_on_p1_no);

  // Participants get ThreadIds 2..N+1.
  for (std::size_t pid = 1; pid <= num_participants; ++pid) {
    prog.threads[participant_to_thread(pid)] =
        make_participant_function(pid);
  }

  return prog;
}

}  // namespace tpc_sim

namespace std {

template <>
struct hash<tpc_sim::SimValue> {
  [[nodiscard]] std::size_t operator()(tpc_sim::SimValue value) const noexcept {
    return hash<std::uint64_t>{}(value.encoded);
  }
};

}  // namespace std
