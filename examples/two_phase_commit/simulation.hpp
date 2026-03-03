#pragma once

// DPOR simulation adapter for Two-Phase Commit.
//
// Bridges the real tpc::Network interface to the DPOR engine by intercepting
// send/receive calls.  Protocol-agnostic: knows nothing about 2PC specifics.
//
// Core idea: each ThreadFunction call with step >= 1 creates a fresh protocol
// object and SimNetwork, launches run() in a real OS thread, fast-forwards
// through past I/O, and captures the current I/O as a DPOR EventLabel.

#include "protocol.hpp"

#include "dpor/algo/program.hpp"
#include "dpor/model/event.hpp"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
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
// SimNetwork: intercepting Network implementation
// ---------------------------------------------------------------------------

class SimNetwork : public tpc::Network {
 public:
  SimNetwork(
      std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map,
      std::size_t target_io,
      const ThreadTrace& trace,
      std::size_t trace_offset)
      : id_map_(std::move(id_map)),
        target_io_(target_io),
        trace_(trace),
        trace_offset_(trace_offset) {}

  void send(tpc::ParticipantId dest, const tpc::Message& msg) override {
    std::unique_lock lock(mu_);
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
    protocol_ready_ = true;
    cv_.notify_all();

    // Wait for DPOR thread to acknowledge (it will tear down the thread).
    cv_.wait(lock, [this] { return dpor_ready_; });
  }

  tpc::Message receive() override {
    std::unique_lock lock(mu_);
    auto current = io_count_++;

    if (current < target_io_) {
      // Fast-forward: past receive, replay value from trace.
      auto idx = trace_offset_ + receive_count_++;
      return tpc::deserialize(trace_.at(idx));
    }

    // This is the target I/O operation.
    result_ = dpor::model::make_receive_label<dpor::model::Value>();
    protocol_ready_ = true;
    cv_.notify_all();

    // Wait for DPOR thread to acknowledge (never returns to protocol).
    cv_.wait(lock, [this] { return dpor_ready_; });

    // Unreachable in practice -- the OS thread gets detached/joined.
    // Return a dummy to satisfy the compiler.
    return tpc::Prepare{};
  }

  // Called by the wrapper after run() returns, indicating the protocol
  // finished before reaching target_io.
  void mark_finished() {
    std::lock_guard lock(mu_);
    finished_ = true;
    protocol_ready_ = true;
    cv_.notify_all();
  }

  // Called by the DPOR thread to wait for the protocol thread to reach
  // the target I/O or finish.
  void wait_for_protocol() {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [this] { return protocol_ready_; });
  }

  // Called by the DPOR thread to release the protocol thread (for teardown).
  void release_protocol() {
    std::lock_guard lock(mu_);
    dpor_ready_ = true;
    cv_.notify_all();
  }

  [[nodiscard]] bool is_finished() const {
    std::lock_guard lock(mu_);
    return finished_;
  }

  [[nodiscard]] std::optional<EventLabel> result() const {
    std::lock_guard lock(mu_);
    return result_;
  }

 private:
  std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map_;
  std::size_t target_io_;
  const ThreadTrace& trace_;
  std::size_t trace_offset_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool protocol_ready_{false};
  bool dpor_ready_{false};

  std::size_t io_count_{0};
  std::size_t receive_count_{0};
  std::optional<EventLabel> result_;
  bool finished_{false};
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

// Run a protocol object in a background OS thread, capturing a single I/O
// event via SimNetwork.  Returns the captured EventLabel or nullopt if the
// protocol finished before reaching the target I/O.
template <typename ProtocolObj>
std::optional<EventLabel> run_and_capture(
    ProtocolObj& obj,
    SimNetwork& net) {
  std::thread worker([&obj, &net] {
    try {
      obj.run(net);
      net.mark_finished();
    } catch (...) {
      net.mark_finished();
    }
  });

  net.wait_for_protocol();

  if (net.is_finished()) {
    worker.join();
    return std::nullopt;
  }

  // Protocol is blocked on the target I/O.  Collect the result
  // and release the thread for cleanup.
  auto label = net.result();
  net.release_protocol();
  worker.join();
  return label;
}

// Create a ThreadFunction for the coordinator.
inline ThreadFunction make_coordinator_function(
    std::size_t num_participants) {
  return [num_participants](const ThreadTrace& trace,
                            std::size_t step) -> std::optional<EventLabel> {
    // Step 0: nondeterministic choice for crash behavior.
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "no_crash",
          .choices = {"no_crash", "crash"},
      };
    }

    // Read the ND choice from trace[0].
    bool crash = (trace.at(0) == "crash");

    tpc::Coordinator coord(num_participants, crash);
    SimNetwork net(participant_to_thread, step - 1, trace, 1);
    return run_and_capture(coord, net);
  };
}

// Create a ThreadFunction for a participant.
inline ThreadFunction make_participant_function(
    tpc::ParticipantId pid) {
  return [pid](const ThreadTrace& trace,
               std::size_t step) -> std::optional<EventLabel> {
    // Step 0: nondeterministic choice for vote.
    if (step == 0) {
      return NondeterministicChoiceLabel{
          .value = "YES",
          .choices = {"YES", "NO"},
      };
    }

    // Read the ND choice from trace[0].
    tpc::Vote vote =
        (trace.at(0) == "YES") ? tpc::Vote::Yes : tpc::Vote::No;

    tpc::Participant participant(pid, vote);
    SimNetwork net(participant_to_thread, step - 1, trace, 1);
    return run_and_capture(participant, net);
  };
}

// ---------------------------------------------------------------------------
// Program assembly
// ---------------------------------------------------------------------------

inline Program make_two_phase_commit_program(
    std::size_t num_participants) {
  Program prog;

  // Coordinator gets ThreadId 1 (= participant_to_thread(kCoordinator)).
  prog.threads[participant_to_thread(tpc::kCoordinator)] =
      make_coordinator_function(num_participants);

  // Participants get ThreadIds 2..N+1.
  for (std::size_t pid = 1; pid <= num_participants; ++pid) {
    prog.threads[participant_to_thread(pid)] =
        make_participant_function(pid);
  }

  return prog;
}

}  // namespace tpc_sim
