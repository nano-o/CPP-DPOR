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

#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace tpc_sim {

using EventLabel = dpor::model::EventLabel;
using SendLabel = dpor::model::SendLabel;
using ReceiveLabel = dpor::model::ReceiveLabel;
using NondeterministicChoiceLabel = dpor::model::NondeterministicChoiceLabel;
using ThreadTrace = dpor::algo::ThreadTrace;
using ThreadFunction = dpor::algo::ThreadFunction;
using Program = dpor::algo::Program;

// ---------------------------------------------------------------------------
// SimEnvironment: intercepting Environment implementation
// ---------------------------------------------------------------------------

// Exception used internally to terminate the protocol thread when a crash
// is injected during fast-forward.
struct CrashInjected {};
struct StepBoundaryReached {};

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
        if (trace_.at(idx) == "crash") {
          throw CrashInjected{};
        }
        // "no_crash": fall through to process the actual send below.
      } else {
        // This is the target I/O: produce ND choice label.
        result_ = NondeterministicChoiceLabel{
            .value = {},
            .choices = {"no_crash", "crash"},
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
        .value = tpc::serialize(msg),
    };
    throw StepBoundaryReached{};
  }

  tpc::Vote get_vote() override {
    auto current = io_count_++;

    if (current < target_io_) {
      // Fast-forward: past vote choice, replay from trace.
      auto idx = trace_offset_ + trace_consume_count_++;
      return trace_.at(idx) == "YES" ? tpc::Vote::Yes : tpc::Vote::No;
    }

    // This is the target I/O operation: produce ND choice label.
    result_ = NondeterministicChoiceLabel{
        .value = {},
        .choices = {"YES", "NO"},
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
      const auto& observed = trace_.at(idx);
      if (observed.is_bottom()) {
        // Timer callbacks execute synchronously during replay. Any send/choice
        // they trigger flows through the normal interceptors below, so it is
        // either fast-forwarded (current < target_io_) or captured as the
        // target event (current == target_io_).
        return SimReceiveResult::replayed_timer_fire(fire_active_timer());
      }
      return SimReceiveResult::replayed_message(
          tpc::deserialize(observed.value()));
    }

    // This is the target I/O operation.
    if (has_active_timer()) {
      result_ =
          dpor::model::make_nonblocking_receive_label<dpor::model::Value>();
    } else {
      result_ = dpor::model::make_receive_label<dpor::model::Value>();
    }
    return SimReceiveResult::captured_receive();
  }

  void set_timer(tpc::TimerId id, std::size_t /*timeout_ms*/,
                 tpc::TimerCallback callback) override {
    // Matching UdpEnvironment, setting the same timer id refreshes/replaces it.
    // A different id would mean multiple simultaneously-active timers, which
    // this simplified adapter does not encode in bottom observations.
    if (active_timer_.has_value() && active_timer_->id != id) {
      throw std::logic_error(
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

  [[nodiscard]] bool fire_active_timer() {
    if (!active_timer_.has_value()) {
      throw std::logic_error(
          "trace requested timer firing but no timer is active");
    }
    // Return the timer callback's continuation flag:
    // true => protocol is still waiting for input
    // false => protocol finished during the callback
    auto callback = std::move(active_timer_->callback);
    active_timer_.reset();
    return callback(*this);
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
    bool needs_message = obj.start(env);
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
      needs_message = obj.receive(env, *receive_step.message);
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
    tpc::Coordinator coord(num_participants, bug_on_p1_no);
    SimEnvironment env(participant_to_thread, step, trace, 0, inject_crash);
    return run_and_capture(coord, env);
  };
}

// Create a ThreadFunction for a participant.
inline ThreadFunction make_participant_function(
    tpc::ParticipantId pid) {
  return [pid](const ThreadTrace& trace,
               std::size_t step) -> std::optional<EventLabel> {
    tpc::Participant participant(pid);
    SimEnvironment env(participant_to_thread, step, trace, 0);
    return run_and_capture(participant, env);
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
