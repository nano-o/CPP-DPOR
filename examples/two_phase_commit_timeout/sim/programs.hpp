#pragma once

#include "../protocol.hpp"
#include "bridge.hpp"
#include "core.hpp"
#include "crash_environment.hpp"
#include "nominal_environment.hpp"

#include <new>
#include <optional>

namespace tpc_sim {

inline ThreadFunction make_nominal_coordinator_function(
    std::size_t num_participants,
    bool bug_on_p1_no = false) {
  return [num_participants, bug_on_p1_no](
             const ThreadTrace& trace,
             std::size_t step) -> std::optional<EventLabel> {
    NondeterministicVoteEnvironment env(participant_to_thread, step, trace, 0);
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

inline ThreadFunction make_crash_before_decision_coordinator_function(
    std::size_t num_participants,
    bool bug_on_p1_no = false) {
  return [num_participants, bug_on_p1_no](
             const ThreadTrace& trace,
             std::size_t step) -> std::optional<EventLabel> {
    CrashBeforeDecisionEnvironment env(participant_to_thread, step, trace, 0);
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

inline ThreadFunction make_participant_function(
    tpc::ParticipantId pid) {
  return [pid](const ThreadTrace& trace,
               std::size_t step) -> std::optional<EventLabel> {
    NondeterministicVoteEnvironment env(participant_to_thread, step, trace, 0);
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

inline Program make_nominal_two_phase_commit_program(
    std::size_t num_participants,
    bool bug_on_p1_no = false) {
  Program prog;
  prog.threads[participant_to_thread(tpc::kCoordinator)] =
      make_nominal_coordinator_function(num_participants, bug_on_p1_no);
  for (std::size_t pid = 1; pid <= num_participants; ++pid) {
    prog.threads[participant_to_thread(pid)] =
        make_participant_function(pid);
  }
  return prog;
}

inline Program make_crash_two_phase_commit_program(
    std::size_t num_participants,
    bool bug_on_p1_no = false) {
  Program prog;
  prog.threads[participant_to_thread(tpc::kCoordinator)] =
      make_crash_before_decision_coordinator_function(num_participants,
                                                      bug_on_p1_no);
  for (std::size_t pid = 1; pid <= num_participants; ++pid) {
    prog.threads[participant_to_thread(pid)] =
        make_participant_function(pid);
  }
  return prog;
}

inline Program make_two_phase_commit_program(
    std::size_t num_participants,
    bool inject_crash = true,
    bool bug_on_p1_no = false) {
  if (inject_crash) {
    return make_crash_two_phase_commit_program(num_participants, bug_on_p1_no);
  }
  return make_nominal_two_phase_commit_program(num_participants, bug_on_p1_no);
}

}  // namespace tpc_sim
