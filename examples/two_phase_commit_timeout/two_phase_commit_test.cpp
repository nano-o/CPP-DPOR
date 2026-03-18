#include "dpor/algo/dpor.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include "sim/bridge.hpp"
#include "sim/crash_before_decision.hpp"
#include "sim/dpor_types.hpp"
#include "sim/nominal.hpp"
#include "udp_network.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dpor;
using namespace tpc_sim;

namespace nominal_sim = tpc_sim::nominal;
namespace crash_sim = tpc_sim::crash_before_decision;

// ---------------------------------------------------------------------------
// Trace helpers
// ---------------------------------------------------------------------------

static std::vector<ExplorationGraph::EventId> thread_event_ids_in_program_order(
    const ExplorationGraph& graph, model::ThreadId tid) {
  std::vector<std::pair<model::EventIndex, ExplorationGraph::EventId>> indexed_events;
  for (ExplorationGraph::EventId id = 0; id < graph.event_count(); ++id) {
    const auto& evt = graph.event(id);
    if (evt.thread == tid) {
      indexed_events.emplace_back(evt.index, id);
    }
  }
  std::sort(indexed_events.begin(), indexed_events.end());

  std::vector<ExplorationGraph::EventId> result;
  result.reserve(indexed_events.size());
  for (const auto& [_, id] : indexed_events) {
    result.push_back(id);
  }
  return result;
}

static std::optional<tpc::Vote> get_participant_vote(const ExplorationGraph& graph,
                                                     tpc::ParticipantId pid) {
  for (const auto id : thread_event_ids_in_program_order(graph, participant_to_thread(pid))) {
    const auto* nd = model::as_nondeterministic_choice(graph.event(id));
    if (nd != nullptr) {
      return decode_vote_choice(nd->value);
    }
  }
  return std::nullopt;
}

static std::optional<tpc::Decision> get_participant_decision(const ExplorationGraph& graph,
                                                             tpc::ParticipantId pid) {
  for (const auto id : thread_event_ids_in_program_order(graph, participant_to_thread(pid))) {
    const auto* recv = model::as_receive(graph.event(id));
    if (recv == nullptr) {
      continue;
    }
    auto rf_it = graph.reads_from().find(id);
    if (rf_it == graph.reads_from().end() || rf_it->second.is_bottom()) {
      continue;
    }
    const auto* send = model::as_send(graph.event(rf_it->second.send_id()));
    if (send == nullptr) {
      continue;
    }
    const auto decision = decode_decision_message(send->value);
    if (decision.has_value()) {
      return decision;
    }
  }
  return std::nullopt;
}

static bool participant_timed_out_locally(const ExplorationGraph& graph, tpc::ParticipantId pid) {
  const auto events = thread_event_ids_in_program_order(graph, participant_to_thread(pid));
  return std::ranges::any_of(events, [&](const auto id) {
    if (model::as_receive(graph.event(id)) == nullptr) {
      return false;
    }
    auto rf_it = graph.reads_from().find(id);
    return rf_it != graph.reads_from().end() && rf_it->second.is_bottom();
  });
}

static bool coordinator_crashed(const ExplorationGraph& graph, std::size_t /*num_participants*/) {
  for (const auto id :
       thread_event_ids_in_program_order(graph, participant_to_thread(tpc::kCoordinator))) {
    const auto* nd = model::as_nondeterministic_choice(graph.event(id));
    if (nd == nullptr) {
      continue;
    }
    if (nd->choices == std::vector<SimValue>{crash_choice(false), crash_choice(true)}) {
      return nd->value == crash_choice(true);
    }
  }
  return false;
}

// Dump the global interleaving of an execution to stderr.
static void dump_global_trace(const ExplorationGraph& graph) {
  for (auto id : graph.insertion_order()) {
    const auto& evt = graph.event(id);
    std::cerr << "  event " << id << " thread=" << evt.thread << " index=" << evt.index;
    if (const auto* send = model::as_send(evt)) {
      std::cerr << " send(dest=" << send->destination << ", val=" << send->value << ")";
    } else if (model::as_receive(evt) != nullptr) {
      // Show which send was matched via reads-from.
      auto it = graph.reads_from().find(id);
      if (it != graph.reads_from().end()) {
        if (it->second.is_bottom()) {
          std::cerr << " receive(bottom)";
        } else {
          const auto& src = graph.event(it->second.send_id());
          if (const auto* s = model::as_send(src)) {
            std::cerr << " receive(val=" << s->value << ")";
          } else {
            std::cerr << " receive(src=" << it->second.send_id() << ")";
          }
        }
      } else {
        std::cerr << " receive(unmatched)";
      }
    } else if (const auto* nd = model::as_nondeterministic_choice(evt)) {
      std::cerr << " nd(val=" << nd->value << ")";
    }
    std::cerr << "\n";
  }
}

// ---------------------------------------------------------------------------
// Coordinator protocol-state-machine tests
// ---------------------------------------------------------------------------

namespace {

struct RecordingEnv : tpc::Environment {
  std::vector<std::pair<tpc::ParticipantId, tpc::Message>> sent;
  tpc::Vote fixed_vote = tpc::Vote::Yes;
  std::unordered_map<tpc::TimerId, tpc::TimerCallback> timers;
  std::size_t set_timer_calls = 0;
  std::size_t cancel_timer_calls = 0;

  void send(tpc::ParticipantId destination, const tpc::Message& msg) override {
    sent.emplace_back(destination, msg);
  }

  tpc::Vote get_vote() override { return fixed_vote; }

  void set_timer(tpc::TimerId id, std::size_t /*timeout_ms*/,
                 tpc::TimerCallback callback) override {
    ++set_timer_calls;
    timers[id] = std::move(callback);
  }

  void cancel_timer(tpc::TimerId id) override {
    ++cancel_timer_calls;
    timers.erase(id);
  }

  std::optional<bool> fire_single_timer() {
    if (timers.size() != 1) {
      return std::nullopt;
    }
    auto it = timers.begin();
    auto callback = std::move(it->second);
    timers.erase(it);
    return callback(*this);
  }
};

std::size_t count_decision_sends(const RecordingEnv& env, tpc::Decision decision) {
  std::size_t count = 0;
  for (const auto& entry : env.sent) {
    const auto& msg = entry.second;
    const auto* dec = std::get_if<tpc::DecisionMsg>(&msg);
    if (dec != nullptr && dec->decision == decision) {
      ++count;
    }
  }
  return count;
}

}  // namespace

TEST_CASE("Coordinator ignores invalid and duplicate votes while collecting votes",
          "[two_phase_commit][protocol]") {
  tpc::Coordinator coord(2);
  RecordingEnv env;

  REQUIRE(coord.start(env));
  REQUIRE(env.sent.size() == 2);  // Prepare to participants 1 and 2.

  // Wrong message kind in vote phase: ignored.
  REQUIRE(coord.receive(env, tpc::Prepare{}));
  REQUIRE(env.sent.size() == 2);

  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::Yes}));
  REQUIRE(env.sent.size() == 2);

  // Duplicate sender vote: ignored.
  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::No}));
  REQUIRE(env.sent.size() == 2);

  // Out-of-range sender: ignored.
  REQUIRE(coord.receive(env, tpc::VoteMsg{3, tpc::Vote::No}));
  REQUIRE(env.sent.size() == 2);

  // Second unique participant vote completes phase 1.
  REQUIRE(coord.receive(env, tpc::VoteMsg{2, tpc::Vote::Yes}));
  REQUIRE(coord.decision() == tpc::Decision::Commit);
  REQUIRE(count_decision_sends(env, tpc::Decision::Commit) == 2);
}

TEST_CASE("Coordinator ignores out-of-phase and duplicate acks", "[two_phase_commit][protocol]") {
  tpc::Coordinator coord(2);
  RecordingEnv env;

  REQUIRE(coord.start(env));

  // Reach ack phase with an Abort decision.
  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::Yes}));
  REQUIRE(coord.receive(env, tpc::VoteMsg{2, tpc::Vote::No}));
  REQUIRE(coord.decision() == tpc::Decision::Abort);
  REQUIRE(count_decision_sends(env, tpc::Decision::Abort) == 2);

  // Vote/decision messages in ack phase are ignored.
  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::Yes}));
  REQUIRE(coord.receive(env, tpc::DecisionMsg{tpc::Decision::Abort}));

  // First ack is accepted.
  REQUIRE(coord.receive(env, tpc::Ack{1}));
  // Duplicate and out-of-range acks are ignored.
  REQUIRE(coord.receive(env, tpc::Ack{1}));
  REQUIRE(coord.receive(env, tpc::Ack{42}));
  // Second unique ack completes protocol.
  REQUIRE_FALSE(coord.receive(env, tpc::Ack{2}));
  REQUIRE_FALSE(coord.receive(env, tpc::Ack{2}));
}

TEST_CASE("Coordinator timeout aborts when not all votes arrive",
          "[two_phase_commit][protocol][timer]") {
  tpc::Coordinator coord(2, /*bug_on_p1_no=*/false,
                         /*vote_timeout_ms=*/10);
  RecordingEnv env;

  REQUIRE(coord.start(env));
  REQUIRE(env.set_timer_calls == 1);
  REQUIRE(env.sent.size() == 2);  // Prepare to participants 1 and 2.

  // Only participant 1 votes; coordinator still waiting for vote 2.
  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::Yes}));
  REQUIRE(coord.decision() == std::nullopt);

  // Timer fires: coordinator must abort and broadcast decision.
  auto keep_running = env.fire_single_timer();
  REQUIRE(keep_running.has_value());
  REQUIRE(*keep_running);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(coord.decision() == tpc::Decision::Abort);
  REQUIRE(count_decision_sends(env, tpc::Decision::Abort) == 2);

  // Late vote is ignored in ack phase.
  REQUIRE(coord.receive(env, tpc::VoteMsg{2, tpc::Vote::Yes}));
  // Protocol completes after unique acks.
  REQUIRE(coord.receive(env, tpc::Ack{1}));
  REQUIRE_FALSE(coord.receive(env, tpc::Ack{2}));
}

TEST_CASE("Coordinator cancels vote timeout after collecting all votes",
          "[two_phase_commit][protocol][timer]") {
  tpc::Coordinator coord(2, /*bug_on_p1_no=*/false,
                         /*vote_timeout_ms=*/10);
  RecordingEnv env;

  REQUIRE(coord.start(env));
  REQUIRE(env.set_timer_calls == 1);  // vote timer

  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::Yes}));
  REQUIRE(coord.receive(env, tpc::VoteMsg{2, tpc::Vote::Yes}));
  REQUIRE(coord.decision() == tpc::Decision::Commit);
  REQUIRE(env.cancel_timer_calls == 1);  // vote timer canceled
  REQUIRE(env.set_timer_calls == 2);     // ack timer armed
  REQUIRE(count_decision_sends(env, tpc::Decision::Commit) == 2);
}

TEST_CASE("Participant arms decision timeout after voting and cancels it on decision",
          "[two_phase_commit][protocol][timer]") {
  tpc::Participant participant(1, /*decision_timeout_ms=*/10);
  RecordingEnv env;
  env.fixed_vote = tpc::Vote::Yes;

  REQUIRE(participant.start(env));
  REQUIRE(env.sent.empty());
  REQUIRE(env.set_timer_calls == 0);

  REQUIRE(participant.receive(env, tpc::Prepare{}));
  REQUIRE(env.set_timer_calls == 1);
  REQUIRE(env.sent.size() == 1);

  const auto* vote = std::get_if<tpc::VoteMsg>(&env.sent.front().second);
  REQUIRE(vote != nullptr);
  REQUIRE(vote->from == 1);
  REQUIRE(vote->vote == tpc::Vote::Yes);

  REQUIRE_FALSE(participant.receive(env, tpc::DecisionMsg{tpc::Decision::Commit}));
  REQUIRE(participant.outcome() == tpc::Decision::Commit);
  REQUIRE(env.cancel_timer_calls == 1);
  REQUIRE_FALSE(env.fire_single_timer().has_value());
  REQUIRE(env.sent.size() == 2);

  const auto* ack = std::get_if<tpc::Ack>(&env.sent.back().second);
  REQUIRE(ack != nullptr);
  REQUIRE(ack->from == 1);
}

TEST_CASE("Coordinator ack timeout completes protocol when participant never acks",
          "[two_phase_commit][protocol][timer]") {
  tpc::Coordinator coord(2, /*bug_on_p1_no=*/false,
                         /*vote_timeout_ms=*/100,
                         /*ack_timeout_ms=*/10);
  RecordingEnv env;

  REQUIRE(coord.start(env));

  // Both participants vote Yes.
  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::Yes}));
  REQUIRE(coord.receive(env, tpc::VoteMsg{2, tpc::Vote::Yes}));
  REQUIRE(coord.decision() == tpc::Decision::Commit);

  // Only participant 1 acks; participant 2 never does.
  REQUIRE(coord.receive(env, tpc::Ack{1}));

  // Ack timer fires — coordinator gives up waiting and completes.
  auto keep_running = env.fire_single_timer();
  REQUIRE(keep_running.has_value());
  REQUIRE_FALSE(*keep_running);  // NOLINT(bugprone-unchecked-optional-access)
  // Coordinator is done despite missing ack from participant 2.
  REQUIRE_FALSE(coord.receive(env, tpc::Ack{2}));
  REQUIRE(coord.decision() == tpc::Decision::Commit);
}

TEST_CASE("Coordinator cancels ack timeout after collecting all acks",
          "[two_phase_commit][protocol][timer]") {
  tpc::Coordinator coord(1, /*bug_on_p1_no=*/false,
                         /*vote_timeout_ms=*/100,
                         /*ack_timeout_ms=*/10);
  RecordingEnv env;

  REQUIRE(coord.start(env));
  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::Yes}));
  REQUIRE(coord.decision() == tpc::Decision::Commit);

  // Ack arrives before timeout.
  REQUIRE_FALSE(coord.receive(env, tpc::Ack{1}));
  // Ack timer was canceled — fire_single_timer returns false.
  REQUIRE_FALSE(env.fire_single_timer().has_value());
}

TEST_CASE("Participant timeout causes local abort while waiting for decision",
          "[two_phase_commit][protocol][timer]") {
  tpc::Participant participant(1, /*decision_timeout_ms=*/10);
  RecordingEnv env;
  env.fixed_vote = tpc::Vote::Yes;

  REQUIRE(participant.start(env));
  REQUIRE(participant.receive(env, tpc::Prepare{}));
  REQUIRE(participant.outcome() == std::nullopt);

  auto keep_running = env.fire_single_timer();
  REQUIRE(keep_running.has_value());
  REQUIRE_FALSE(*keep_running);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(participant.outcome() == tpc::Decision::Abort);

  // Once the timeout fires, the participant stays aborted even if a late
  // decision is delivered.
  REQUIRE_FALSE(participant.receive(env, tpc::DecisionMsg{tpc::Decision::Commit}));
  REQUIRE(participant.outcome() == tpc::Decision::Abort);
}

// ---------------------------------------------------------------------------
// Simulation adapter tests
// ---------------------------------------------------------------------------

TEST_CASE("nominal::Environment captures timer-free waits as blocking receives",
          "[two_phase_commit][simulation][timer]") {
  struct WaitForMessage {
    static bool start(tpc::Environment& /*env*/) { return true; }
    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  WaitForMessage protocol;
  ThreadTrace trace;
  nominal_sim::Environment env(participant_to_thread, /*target_io=*/0, trace, /*trace_offset=*/0);

  const auto label = nominal_sim::run_and_capture(protocol, env);
  REQUIRE(label.has_value());
  const auto* recv =
      std::get_if<ReceiveLabel>(&*label);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(recv != nullptr);
  REQUIRE(recv->is_blocking());
}

TEST_CASE("nominal::Environment captures timer-armed waits as non-blocking receives",
          "[two_phase_commit][simulation][timer]") {
  struct WaitWithTimer {
    static bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [](tpc::Environment& /*timer_env*/) { return false; });
      return true;
    }
    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  WaitWithTimer protocol;
  ThreadTrace trace;
  nominal_sim::Environment env(participant_to_thread, /*target_io=*/0, trace, /*trace_offset=*/0);

  const auto label = nominal_sim::run_and_capture(protocol, env);
  REQUIRE(label.has_value());
  const auto* recv =
      std::get_if<ReceiveLabel>(&*label);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(recv != nullptr);
  REQUIRE(recv->is_nonblocking());
}

TEST_CASE("nominal::Environment returns to blocking receive after timer cancellation",
          "[two_phase_commit][simulation][timer]") {
  struct WaitWithCanceledTimer {
    static bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [](tpc::Environment& /*timer_env*/) { return false; });
      env.cancel_timer(1);
      return true;
    }
    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  WaitWithCanceledTimer protocol;
  ThreadTrace trace;
  nominal_sim::Environment env(participant_to_thread, /*target_io=*/0, trace, /*trace_offset=*/0);

  const auto label = nominal_sim::run_and_capture(protocol, env);
  REQUIRE(label.has_value());
  const auto* recv =
      std::get_if<ReceiveLabel>(&*label);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(recv != nullptr);
  REQUIRE(recv->is_blocking());
}

TEST_CASE("nominal::Environment replays bottom as timer firing",
          "[two_phase_commit][simulation][timer]") {
  struct TimerThenSend {
    bool timer_fired = false;

    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment& timer_env) {
        timer_fired = true;
        timer_env.send(1, tpc::Prepare{});
        return false;
      });
      return true;
    }

    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  TimerThenSend protocol;
  ThreadTrace trace{ObservedValue::bottom()};
  nominal_sim::Environment env(participant_to_thread, /*target_io=*/1, trace, /*trace_offset=*/0);

  const auto label = nominal_sim::run_and_capture(protocol, env);
  REQUIRE(protocol.timer_fired);
  REQUIRE(label.has_value());
  const auto* send = std::get_if<SendLabel>(&*label);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(send != nullptr);
  REQUIRE(send->destination == participant_to_thread(1));
  REQUIRE(send->value == prepare_message());
}

TEST_CASE("nominal::Environment replays timer-callback sends before later target steps",
          "[two_phase_commit][simulation][timer]") {
  struct TimerSendThenWait {
    bool timer_fired = false;

    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment& timer_env) {
        timer_fired = true;
        timer_env.send(1, tpc::Prepare{});
        return true;
      });
      return true;
    }

    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  TimerSendThenWait protocol;
  ThreadTrace trace{ObservedValue::bottom()};
  nominal_sim::Environment env(participant_to_thread, /*target_io=*/2, trace, /*trace_offset=*/0);

  const auto label = nominal_sim::run_and_capture(protocol, env);
  REQUIRE(protocol.timer_fired);
  REQUIRE(label.has_value());
  const auto* recv =
      std::get_if<ReceiveLabel>(&*label);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(recv != nullptr);
  REQUIRE(recv->is_blocking());
}

TEST_CASE("nominal::Environment refreshes the active timer when the id is reused",
          "[two_phase_commit][simulation][timer]") {
  struct ReplaceTimer {
    bool old_timer_fired = false;
    bool new_timer_fired = false;

    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment& timer_env) {
        old_timer_fired = true;
        timer_env.send(1, tpc::Prepare{});
        return false;
      });
      env.set_timer(1, 20, [this](tpc::Environment& timer_env) {
        new_timer_fired = true;
        timer_env.send(1, tpc::Ack{1});
        return false;
      });
      return true;
    }

    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  ReplaceTimer protocol;
  ThreadTrace trace{ObservedValue::bottom()};
  nominal_sim::Environment env(participant_to_thread, /*target_io=*/1, trace, /*trace_offset=*/0);

  const auto label = nominal_sim::run_and_capture(protocol, env);
  REQUIRE_FALSE(protocol.old_timer_fired);
  REQUIRE(protocol.new_timer_fired);
  REQUIRE(label.has_value());
  const auto* send = std::get_if<SendLabel>(&*label);  // NOLINT(bugprone-unchecked-optional-access)
  REQUIRE(send != nullptr);
  REQUIRE(send->destination == participant_to_thread(1));
  REQUIRE(send->value == ack_message(1));
}

TEST_CASE(
    "nominal::Environment rejects multiple simultaneous active timers as a simulation failure",
    "[two_phase_commit][simulation][timer]") {
  struct WaitWithTwoTimers {
    static bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [](tpc::Environment& /*timer_env*/) { return true; });
      env.set_timer(2, 10, [](tpc::Environment& /*timer_env*/) { return false; });
      return true;
    }

    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  WaitWithTwoTimers protocol;
  ThreadTrace trace;
  nominal_sim::Environment env(participant_to_thread, /*target_io=*/0, trace, /*trace_offset=*/0);

  // The simplified timer adapter cannot represent multiple concurrent timers,
  // so this remains an infrastructure failure rather than an ErrorLabel.
  REQUIRE_THROWS_AS(nominal_sim::run_and_capture(protocol, env), std::logic_error);
}

TEST_CASE("nominal::Environment turns timer-callback protocol exceptions into error events",
          "[two_phase_commit][simulation][timer]") {
  struct ThrowInTimer {
    static bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [](tpc::Environment& /*timer_env*/) -> bool {
        throw std::logic_error("protocol bug");
      });
      return true;
    }

    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  ThrowInTimer protocol;
  ThreadTrace trace{ObservedValue::bottom()};
  nominal_sim::Environment env(participant_to_thread, /*target_io=*/1, trace, /*trace_offset=*/0);

  const auto label = nominal_sim::run_and_capture(protocol, env);
  if (!label.has_value()) {
    FAIL("expected error label");
    return;
  }
  const auto& actual_label = *label;
  REQUIRE(std::holds_alternative<model::ErrorLabel>(actual_label));
  REQUIRE(std::get<model::ErrorLabel>(actual_label).message == "protocol bug");
}

// ---------------------------------------------------------------------------
// DPOR tests
// ---------------------------------------------------------------------------

TEST_CASE("2PC basic exploration with 2 participants", "[two_phase_commit]") {
  auto prog = crash_sim::make_program({.num_participants = 2});

  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  // The old 4 vote-combos * 4 delivery-orderings accounting no longer applies:
  // every timer-armed wait adds a bottom branch alongside message matches.
  // For 2 participants this timer-inclusive model explores 876 no-crash
  // executions, plus 20 additional crash branches at the decision boundary.
  REQUIRE(result.executions_explored == 896);
}

TEST_CASE("2PC DPOR explores participant local timeout executions", "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = nominal_sim::make_program({.num_participants = kNumParticipants});

  bool saw_local_timeout = false;

  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);
  config.on_execution = [&](const ExplorationGraph& graph) {
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      if (participant_timed_out_locally(graph, pid)) {
        saw_local_timeout = true;
      }
    }
  };

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  REQUIRE(saw_local_timeout);
}

TEST_CASE("2PC agreement invariant: all decided participants agree", "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = crash_sim::make_program({.num_participants = kNumParticipants});

  bool invariant_violated = false;

  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);
  config.on_execution = [&](const ExplorationGraph& graph) {
    if (coordinator_crashed(graph, kNumParticipants)) {
      return;
    }

    // Collect decisions from all participants that received one.
    std::set<tpc::Decision> decisions;
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      auto dec = get_participant_decision(graph, pid);
      if (dec.has_value()) {
        decisions.insert(*dec);
      }
    }

    // Agreement: at most one distinct decision value.
    if (decisions.size() > 1) {
      invariant_violated = true;
    }
  };

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  REQUIRE_FALSE(invariant_violated);
}

TEST_CASE("2PC validity invariant: Commit implies all voted Yes", "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = crash_sim::make_program({.num_participants = kNumParticipants});

  bool invariant_violated = false;

  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);
  config.on_execution = [&](const ExplorationGraph& graph) {
    if (coordinator_crashed(graph, kNumParticipants)) {
      return;
    }

    // Check if any participant got a Commit decision.
    bool has_commit = false;
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      auto dec = get_participant_decision(graph, pid);
      if (dec == tpc::Decision::Commit) {
        has_commit = true;
      }
    }

    if (!has_commit) {
      return;
    }

    // If Commit was decided, all participants must have voted Yes.
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      if (get_participant_vote(graph, pid) != tpc::Vote::Yes) {
        invariant_violated = true;
      }
    }
  };

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  REQUIRE_FALSE(invariant_violated);
}

TEST_CASE("2PC crash behavior: no participant decides after coordinator crash",
          "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = crash_sim::make_program({.num_participants = kNumParticipants});

  bool invariant_violated = false;
  std::size_t crash_executions = 0;

  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);
  config.on_execution = [&](const ExplorationGraph& graph) {
    if (!coordinator_crashed(graph, kNumParticipants)) {
      return;
    }

    ++crash_executions;

    // When coordinator crashes between phases, no participant should
    // receive a decision.
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      auto dec = get_participant_decision(graph, pid);
      if (dec.has_value()) {
        invariant_violated = true;
      }
    }
  };

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  REQUIRE(crash_executions > 0);
  REQUIRE_FALSE(invariant_violated);
}

TEST_CASE("2PC without crashes explores timeout-inclusive executions for 2 participants",
          "[two_phase_commit]") {
  auto prog = nominal_sim::make_program({.num_participants = 2});

  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  // Regression baseline for the timer-inclusive no-crash state space.
  REQUIRE(result.executions_explored == 876);
}

TEST_CASE("2PC scales to 3 participants", "[two_phase_commit]") {
  auto prog = crash_sim::make_program({.num_participants = 3});

  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored > 0);
}

TEST_CASE("2PC protocol bug surfaces as verification failure", "[two_phase_commit]") {
  auto prog = nominal_sim::make_program({
      .num_participants = 2,
      .bug_on_p1_no = true,
  });

  bool saw_error_execution = false;
  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);
  config.on_execution = [&](const ExplorationGraph& graph) {
    for (std::size_t event_id = 0; event_id < graph.event_count(); ++event_id) {
      if (model::is_error(graph.event(event_id))) {
        saw_error_execution = true;
      }
    }
  };

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::ErrorFound);
  REQUIRE(result.message.find("coordinator cannot handle No vote from participant 1") !=
          std::string::npos);
  REQUIRE(saw_error_execution);
}

TEST_CASE("2PC false invariant is detected: Abort implies some voted No", "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = nominal_sim::make_program({.num_participants = kNumParticipants});

  bool invariant_violated = false;

  algo::DporConfigT<SimValue> config;
  config.program = std::move(prog);
  config.on_execution = [&](const ExplorationGraph& graph) {
    // False invariant: "if the decision is Abort, then at least one
    // participant voted No."  This is actually true for 2PC, so let's
    // check the *opposite*: "Abort never happens."  Since participants
    // can vote No, this must be violated in at least one execution.
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      auto dec = get_participant_decision(graph, pid);
      if (dec == tpc::Decision::Abort) {
        if (!invariant_violated) {
          std::cerr << "Invariant violated! Global trace:\n";
          dump_global_trace(graph);
        }
        invariant_violated = true;
      }
    }
  };

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  // The false invariant ("Abort never happens") must be violated.
  REQUIRE(invariant_violated);
}

// ---------------------------------------------------------------------------
// UDP tests
// ---------------------------------------------------------------------------

// Allocate an ephemeral port by binding to port 0, reading the assigned port,
// and closing the socket.  There is a small TOCTOU window, but it is fine for
// tests on localhost.
static uint16_t allocate_ephemeral_port() {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  REQUIRE(fd >= 0);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  REQUIRE(
      ::bind(fd,
             reinterpret_cast<sockaddr*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                 &addr),
             sizeof(addr)) == 0);

  socklen_t len = sizeof(addr);
  REQUIRE(::getsockname(
              fd,
              reinterpret_cast<sockaddr*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                  &addr),
              &len) == 0);
  auto port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

using PortMap = std::unordered_map<tpc::ParticipantId, std::pair<std::string, uint16_t>>;

// Build a port map for coordinator + N participants on localhost.
static PortMap make_localhost_port_map(std::size_t num_participants) {
  PortMap pm;
  for (std::size_t id = 0; id <= num_participants; ++id) {
    pm[id] = {"127.0.0.1", allocate_ephemeral_port()};
  }
  return pm;
}

TEST_CASE("UDP: serialize/deserialize roundtrip for all message types", "[two_phase_commit][udp]") {
  auto check = [](const tpc::Message& original) {
    std::string data = tpc::serialize(original);
    tpc::Message recovered = tpc::deserialize(data);
    REQUIRE(recovered.index() == original.index());
  };

  check(tpc::Prepare{});
  check(tpc::VoteMsg{1, tpc::Vote::Yes});
  check(tpc::VoteMsg{2, tpc::Vote::No});
  check(tpc::DecisionMsg{tpc::Decision::Commit});
  check(tpc::DecisionMsg{tpc::Decision::Abort});
  check(tpc::Ack{3});
}

TEST_CASE("UDP: send and receive a single message over localhost", "[two_phase_commit][udp]") {
  auto pm = make_localhost_port_map(1);

  tpc::UdpEnvironment sender(tpc::kCoordinator, pm);
  tpc::UdpEnvironment receiver(1, pm);

  // Use a trivial one-message protocol to test send/receive.
  struct OneReceiver {
    tpc::Message received;
    static bool start(tpc::Environment& /*env*/) { return true; }
    bool receive(tpc::Environment& /*env*/, const tpc::Message& msg) {
      received = msg;
      return false;
    }
  };

  tpc::Message sent = tpc::Prepare{};
  sender.send(1, sent);

  OneReceiver proto;
  receiver.run(proto);
  REQUIRE(std::holds_alternative<tpc::Prepare>(proto.received));
}

TEST_CASE("UDP: send and receive VoteMsg preserves fields", "[two_phase_commit][udp]") {
  auto pm = make_localhost_port_map(1);

  tpc::UdpEnvironment participant_env(1, pm, tpc::Vote::Yes);
  tpc::UdpEnvironment coordinator_env(tpc::kCoordinator, pm);

  participant_env.send(tpc::kCoordinator, tpc::VoteMsg{1, tpc::Vote::Yes});

  struct OneReceiver {
    tpc::Message received;
    static bool start(tpc::Environment& /*env*/) { return true; }
    bool receive(tpc::Environment& /*env*/, const tpc::Message& msg) {
      received = msg;
      return false;
    }
  };

  OneReceiver proto;
  coordinator_env.run(proto);
  const auto* vote = std::get_if<tpc::VoteMsg>(&proto.received);
  REQUIRE(vote != nullptr);
  REQUIRE(vote->from == 1);
  REQUIRE(vote->vote == tpc::Vote::Yes);
}

TEST_CASE("UDP: full 2PC protocol run with all-Yes votes commits", "[two_phase_commit][udp]") {
  constexpr std::size_t kN = 2;
  auto pm = make_localhost_port_map(kN);

  tpc::Coordinator coord(kN);
  tpc::Participant p1(1);
  tpc::Participant p2(2);

  // Create all environments on the main thread so every socket is bound
  // before any thread starts sending.
  tpc::UdpEnvironment env_coord(tpc::kCoordinator, pm);
  tpc::UdpEnvironment env_p1(1, pm, tpc::Vote::Yes);
  tpc::UdpEnvironment env_p2(2, pm, tpc::Vote::Yes);

  std::thread t_coord([&] { env_coord.run(coord); });
  std::thread t_p1([&] { env_p1.run(p1); });
  std::thread t_p2([&] { env_p2.run(p2); });

  t_coord.join();
  t_p1.join();
  t_p2.join();

  REQUIRE(coord.decision() == tpc::Decision::Commit);
  REQUIRE(p1.outcome() == tpc::Decision::Commit);
  REQUIRE(p2.outcome() == tpc::Decision::Commit);
}

TEST_CASE("UDP: full 2PC protocol run with a No vote aborts", "[two_phase_commit][udp]") {
  constexpr std::size_t kN = 2;
  auto pm = make_localhost_port_map(kN);

  tpc::Coordinator coord(kN);
  tpc::Participant p1(1);
  tpc::Participant p2(2);

  tpc::UdpEnvironment env_coord(tpc::kCoordinator, pm);
  tpc::UdpEnvironment env_p1(1, pm, tpc::Vote::Yes);
  tpc::UdpEnvironment env_p2(2, pm, tpc::Vote::No);

  std::thread t_coord([&] { env_coord.run(coord); });
  std::thread t_p1([&] { env_p1.run(p1); });
  std::thread t_p2([&] { env_p2.run(p2); });

  t_coord.join();
  t_p1.join();
  t_p2.join();

  REQUIRE(coord.decision() == tpc::Decision::Abort);
  REQUIRE(p1.outcome() == tpc::Decision::Abort);
  REQUIRE(p2.outcome() == tpc::Decision::Abort);
}

TEST_CASE("UDP: repeated random-vote runs satisfy agreement", "[two_phase_commit][udp]") {
  constexpr std::size_t kN = 2;
  constexpr int kRuns = 20;

  for (int run = 0; run < kRuns; ++run) {
    auto pm = make_localhost_port_map(kN);

    tpc::Coordinator coord(kN);
    tpc::Participant p1(1);
    tpc::Participant p2(2);

    // No vote argument — each participant picks a random vote.
    tpc::UdpEnvironment env_coord(tpc::kCoordinator, pm);
    tpc::UdpEnvironment env_p1(1, pm);
    tpc::UdpEnvironment env_p2(2, pm);

    std::thread t_coord([&] { env_coord.run(coord); });
    std::thread t_p1([&] { env_p1.run(p1); });
    std::thread t_p2([&] { env_p2.run(p2); });

    t_coord.join();
    t_p1.join();
    t_p2.join();

    // All nodes must have reached a decision.
    REQUIRE(coord.decision().has_value());
    REQUIRE(p1.outcome().has_value());
    REQUIRE(p2.outcome().has_value());

    // Agreement: every participant got the same decision as the coordinator.
    REQUIRE(p1.outcome() == coord.decision());
    REQUIRE(p2.outcome() == coord.decision());
  }
}

TEST_CASE("UDP: run() is single-use per environment", "[two_phase_commit][udp]") {
  constexpr std::size_t kN = 1;
  auto pm = make_localhost_port_map(kN);

  tpc::Coordinator coord(kN);
  tpc::Participant p1(1);

  tpc::UdpEnvironment env_coord(tpc::kCoordinator, pm);
  tpc::UdpEnvironment env_p1(1, pm, tpc::Vote::Yes);

  std::thread t_coord([&] { env_coord.run(coord); });
  std::thread t_p1([&] { env_p1.run(p1); });

  t_coord.join();
  t_p1.join();

  tpc::Coordinator coord2(kN);
  REQUIRE_THROWS_AS(env_coord.run(coord2), std::logic_error);
}

TEST_CASE("UDP: malformed datagrams are skipped without crashing", "[two_phase_commit][udp]") {
  constexpr std::size_t kN = 1;
  auto pm = make_localhost_port_map(kN);

  tpc::Coordinator coord(kN);
  tpc::Participant p1(1);

  tpc::UdpEnvironment env_coord(tpc::kCoordinator, pm);
  tpc::UdpEnvironment env_p1(1, pm, tpc::Vote::Yes);

  // Send garbage to the coordinator before starting the protocol.
  // It should be silently discarded by the receiver thread.
  int raw_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  REQUIRE(raw_fd >= 0);
  sockaddr_in coord_addr{};
  coord_addr.sin_family = AF_INET;
  coord_addr.sin_port = htons(pm[tpc::kCoordinator].second);
  ::inet_pton(AF_INET, "127.0.0.1", &coord_addr.sin_addr);

  const char* junk = "NOT_A_VALID_MESSAGE !!!";
  auto sent =
      ::sendto(raw_fd, junk, std::strlen(junk), 0,
               reinterpret_cast<sockaddr*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                   &coord_addr),
               sizeof(coord_addr));
  ::close(raw_fd);
  REQUIRE(sent == static_cast<ssize_t>(std::strlen(junk)));

  // The protocol should still complete normally despite the junk datagram.
  std::thread t_coord([&] { env_coord.run(coord); });
  std::thread t_p1([&] { env_p1.run(p1); });

  t_coord.join();
  t_p1.join();

  REQUIRE(coord.decision() == tpc::Decision::Commit);
  REQUIRE(p1.outcome() == tpc::Decision::Commit);
}

// ---------------------------------------------------------------------------
// Timer tests
// ---------------------------------------------------------------------------

TEST_CASE("UDP: timer fires without incoming UDP", "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  // Protocol that sets a timer on start and finishes when the timer fires.
  struct TimerProto {
    bool fired = false;
    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment&) {
        fired = true;
        return false;
      });
      return true;
    }
    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  tpc::UdpEnvironment env(tpc::kCoordinator, pm);
  TimerProto proto{};
  env.run(proto);
  REQUIRE(proto.fired);
}

TEST_CASE("UDP: canceled timer does not fire", "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  struct CancelProto {
    bool timer_fired = false;

    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment&) {
        timer_fired = true;
        return true;
      });
      env.cancel_timer(1);
      // Set a second timer to end the protocol.
      env.set_timer(2, 30, [this](tpc::Environment&) { return false; });
      return true;
    }
    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  tpc::UdpEnvironment env(tpc::kCoordinator, pm);
  CancelProto proto{};
  env.run(proto);
  REQUIRE_FALSE(proto.timer_fired);
}

TEST_CASE("UDP: replace same id fires only newest callback", "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  struct ReplaceProto {
    int which_fired = 0;

    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment&) {
        which_fired = 1;
        return true;
      });
      // Replace with new callback.
      env.set_timer(1, 10, [this](tpc::Environment&) {
        which_fired = 2;
        return false;
      });
      return true;
    }
    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  tpc::UdpEnvironment env(tpc::kCoordinator, pm);
  ReplaceProto proto{};
  env.run(proto);
  REQUIRE(proto.which_fired == 2);
}

TEST_CASE("UDP: shutdown with pending timers exits cleanly", "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  struct ShutdownProto {
    static bool start(tpc::Environment& env) {
      // Set a long timer that should never fire.
      env.set_timer(1, 60000, [](tpc::Environment&) { return true; });
      return false;
    }
    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) { return false; }
  };

  tpc::UdpEnvironment env(tpc::kCoordinator, pm);
  ShutdownProto proto{};
  // Should return promptly despite the 60s pending timer.
  env.run(proto);
  // If we get here, shutdown was clean.
  REQUIRE(true);
}

TEST_CASE("UDP: participant locally aborts when decision timer fires",
          "[two_phase_commit][udp][timer]") {
  // Use 2 participants but only run participant 1. The coordinator waits for
  // both votes and won't decide until its vote timeout fires (200ms). Since
  // participant 1's decision timeout (10ms) is much shorter, participant 1
  // times out and locally aborts before the coordinator ever sends a decision.
  auto pm = make_localhost_port_map(2);

  tpc::Coordinator coord(2, /*bug_on_p1_no=*/false,
                         /*vote_timeout_ms=*/200,
                         /*ack_timeout_ms=*/200);
  tpc::Participant participant(1, /*decision_timeout_ms=*/10);

  tpc::UdpEnvironment env_coord(tpc::kCoordinator, pm);
  tpc::UdpEnvironment env_p1(1, pm, tpc::Vote::Yes);

  std::thread t_coord([&] { env_coord.run(coord); });
  std::thread t_p1([&] { env_p1.run(participant); });

  t_p1.join();
  t_coord.join();

  // Participant timed out waiting for a decision.
  REQUIRE(participant.outcome() == tpc::Decision::Abort);
  // Coordinator eventually decided Abort (vote timeout, missing participant 2).
  REQUIRE(coord.decision() == tpc::Decision::Abort);
}
