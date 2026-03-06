#include "simulation.hpp"
#include "udp_network.hpp"

#include "dpor/algo/dpor.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dpor;
using namespace tpc_sim;

// ---------------------------------------------------------------------------
// Trace helpers
// ---------------------------------------------------------------------------

// Participant trace layout (program order):
//   [0] = received Prepare (serialized)
//   [1] = vote ND choice ("YES" / "NO")
//   [2] = received DecisionMsg (serialized) -- only if coordinator didn't crash
static std::string get_participant_vote(
    const model::ExplorationGraph& graph,
    tpc::ParticipantId pid) {
  auto trace = graph.thread_trace(participant_to_thread(pid));
  if (trace.size() >= 2) {
    return trace[1].value();
  }
  return {};
}

static std::optional<std::string> get_participant_decision(
    const model::ExplorationGraph& graph,
    tpc::ParticipantId pid) {
  auto trace = graph.thread_trace(participant_to_thread(pid));
  if (trace.size() >= 3) {
    return trace[2].value();
  }
  return std::nullopt;
}

// Coordinator trace layout (program order, with N participants):
//   [0..N-1] = received votes (serialized VoteMsg)
//   [N]      = crash ND choice ("no_crash" / "crash")
static bool coordinator_crashed(const model::ExplorationGraph& graph,
                                std::size_t num_participants) {
  auto trace =
      graph.thread_trace(participant_to_thread(tpc::kCoordinator));
  if (trace.size() > num_participants) {
    return trace[num_participants] == "crash";
  }
  return false;
}

// Dump the global interleaving of an execution to stderr.
static void dump_global_trace(const model::ExplorationGraph& graph) {
  for (auto id : graph.insertion_order()) {
    const auto& evt = graph.event(id);
    std::cerr << "  event " << id
              << " thread=" << evt.thread
              << " index=" << evt.index;
    if (const auto* send = model::as_send(evt)) {
      std::cerr << " send(dest=" << send->destination
                << ", val=" << send->value << ")";
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

  bool fire_single_timer() {
    if (timers.size() != 1) {
      return false;
    }
    auto it = timers.begin();
    auto callback = std::move(it->second);
    timers.erase(it);
    callback(*this);
    return true;
  }

};

static std::size_t count_decision_sends(const RecordingEnv& env,
                                        tpc::Decision decision) {
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

TEST_CASE(
    "Coordinator ignores invalid and duplicate votes while collecting votes",
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

TEST_CASE("Coordinator ignores out-of-phase and duplicate acks",
          "[two_phase_commit][protocol]") {
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
  REQUIRE(env.fire_single_timer());
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
  REQUIRE(env.set_timer_calls == 1);

  REQUIRE(coord.receive(env, tpc::VoteMsg{1, tpc::Vote::Yes}));
  REQUIRE(coord.receive(env, tpc::VoteMsg{2, tpc::Vote::Yes}));
  REQUIRE(coord.decision() == tpc::Decision::Commit);
  REQUIRE(env.cancel_timer_calls == 1);
  REQUIRE_FALSE(env.fire_single_timer());
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

  REQUIRE_FALSE(
      participant.receive(env, tpc::DecisionMsg{tpc::Decision::Commit}));
  REQUIRE(participant.outcome() == tpc::Decision::Commit);
  REQUIRE(env.cancel_timer_calls == 1);
  REQUIRE_FALSE(env.fire_single_timer());
  REQUIRE(env.sent.size() == 2);

  const auto* ack = std::get_if<tpc::Ack>(&env.sent.back().second);
  REQUIRE(ack != nullptr);
  REQUIRE(ack->from == 1);
}

TEST_CASE("Participant timeout causes local abort while waiting for decision",
          "[two_phase_commit][protocol][timer]") {
  tpc::Participant participant(1, /*decision_timeout_ms=*/10);
  RecordingEnv env;
  env.fixed_vote = tpc::Vote::Yes;

  REQUIRE(participant.start(env));
  REQUIRE(participant.receive(env, tpc::Prepare{}));
  REQUIRE(participant.outcome() == std::nullopt);

  REQUIRE(env.fire_single_timer());
  REQUIRE(participant.outcome() == tpc::Decision::Abort);

  // Once the timeout fires, the participant stays aborted even if a late
  // decision is delivered.
  REQUIRE_FALSE(
      participant.receive(env, tpc::DecisionMsg{tpc::Decision::Commit}));
  REQUIRE(participant.outcome() == tpc::Decision::Abort);
}

// ---------------------------------------------------------------------------
// DPOR tests
// ---------------------------------------------------------------------------

TEST_CASE("2PC basic exploration with 2 participants",
          "[two_phase_commit]") {
  auto prog = make_two_phase_commit_program(2);

  algo::DporConfig config;
  config.program = std::move(prog);

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  // No-crash: 4 vote combos * 4 orderings = 16
  // Crash:    4 vote combos * 2 orderings = 8
  // Total: 24
  REQUIRE(result.executions_explored == 24);
}

TEST_CASE("2PC agreement invariant: all decided participants agree",
          "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = make_two_phase_commit_program(kNumParticipants);

  bool invariant_violated = false;

  algo::DporConfig config;
  config.program = std::move(prog);
  config.on_execution = [&](const model::ExplorationGraph& graph) {
    if (coordinator_crashed(graph, kNumParticipants)) {
      return;
    }

    // Collect decisions from all participants that received one.
    std::set<std::string> decisions;
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

TEST_CASE("2PC validity invariant: Commit implies all voted Yes",
          "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = make_two_phase_commit_program(kNumParticipants);

  bool invariant_violated = false;

  algo::DporConfig config;
  config.program = std::move(prog);
  config.on_execution = [&](const model::ExplorationGraph& graph) {
    if (coordinator_crashed(graph, kNumParticipants)) {
      return;
    }

    // Check if any participant got a Commit decision.
    bool has_commit = false;
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      auto dec = get_participant_decision(graph, pid);
      if (dec.has_value() && *dec == "DECISION COMMIT") {
        has_commit = true;
      }
    }

    if (!has_commit) {
      return;
    }

    // If Commit was decided, all participants must have voted Yes.
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      if (get_participant_vote(graph, pid) != "YES") {
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
  auto prog = make_two_phase_commit_program(kNumParticipants);

  bool invariant_violated = false;
  std::size_t crash_executions = 0;

  algo::DporConfig config;
  config.program = std::move(prog);
  config.on_execution = [&](const model::ExplorationGraph& graph) {
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

TEST_CASE("2PC without crashes explores 16 executions for 2 participants",
          "[two_phase_commit]") {
  auto prog = make_two_phase_commit_program(2, /*inject_crash=*/false);

  algo::DporConfig config;
  config.program = std::move(prog);

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  // 4 vote combinations (2x2) * 4 message delivery orderings = 16
  REQUIRE(result.executions_explored == 16);
}

TEST_CASE("2PC scales to 3 participants", "[two_phase_commit]") {
  auto prog = make_two_phase_commit_program(3);

  algo::DporConfig config;
  config.program = std::move(prog);

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored > 0);
}

TEST_CASE("2PC protocol bug surfaces as exception, not silent success",
          "[two_phase_commit]") {
  auto prog = make_two_phase_commit_program(2, /*inject_crash=*/false,
                                            /*bug_on_p1_no=*/true);

  algo::DporConfig config;
  config.program = std::move(prog);

  // The buggy coordinator throws when participant 1 votes No.
  // This must propagate out of verify(), not be silently swallowed.
  REQUIRE_THROWS_AS(algo::verify(config), std::logic_error);
}

TEST_CASE("2PC false invariant is detected: Abort implies some voted No",
          "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = make_two_phase_commit_program(kNumParticipants,
                                            /*inject_crash=*/false);

  bool invariant_violated = false;

  algo::DporConfig config;
  config.program = std::move(prog);
  config.on_execution = [&](const model::ExplorationGraph& graph) {
    // False invariant: "if the decision is Abort, then at least one
    // participant voted No."  This is actually true for 2PC, so let's
    // check the *opposite*: "Abort never happens."  Since participants
    // can vote No, this must be violated in at least one execution.
    for (std::size_t pid = 1; pid <= kNumParticipants; ++pid) {
      auto dec = get_participant_decision(graph, pid);
      if (dec.has_value() && *dec == "DECISION ABORT") {
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
  REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  socklen_t len = sizeof(addr);
  REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  auto port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

using PortMap =
    std::unordered_map<tpc::ParticipantId, std::pair<std::string, uint16_t>>;

// Build a port map for coordinator + N participants on localhost.
static PortMap make_localhost_port_map(std::size_t num_participants) {
  PortMap pm;
  for (std::size_t id = 0; id <= num_participants; ++id) {
    pm[id] = {"127.0.0.1", allocate_ephemeral_port()};
  }
  return pm;
}

TEST_CASE("UDP: serialize/deserialize roundtrip for all message types",
          "[two_phase_commit][udp]") {
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

TEST_CASE("UDP: send and receive a single message over localhost",
          "[two_phase_commit][udp]") {
  auto pm = make_localhost_port_map(1);

  tpc::UdpEnvironment sender(tpc::kCoordinator, pm);
  tpc::UdpEnvironment receiver(1, pm);

  // Use a trivial one-message protocol to test send/receive.
  struct OneReceiver {
    tpc::Message received;
    bool start(tpc::Environment& /*env*/) { return true; }
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

TEST_CASE("UDP: send and receive VoteMsg preserves fields",
          "[two_phase_commit][udp]") {
  auto pm = make_localhost_port_map(1);

  tpc::UdpEnvironment participant_env(1, pm, tpc::Vote::Yes);
  tpc::UdpEnvironment coordinator_env(tpc::kCoordinator, pm);

  participant_env.send(tpc::kCoordinator,
                       tpc::VoteMsg{1, tpc::Vote::Yes});

  struct OneReceiver {
    tpc::Message received;
    bool start(tpc::Environment& /*env*/) { return true; }
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

TEST_CASE("UDP: full 2PC protocol run with all-Yes votes commits",
          "[two_phase_commit][udp]") {
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

TEST_CASE("UDP: full 2PC protocol run with a No vote aborts",
          "[two_phase_commit][udp]") {
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

TEST_CASE("UDP: repeated random-vote runs satisfy agreement",
          "[two_phase_commit][udp]") {
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

TEST_CASE("UDP: run() is single-use per environment",
          "[two_phase_commit][udp]") {
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

TEST_CASE("UDP: malformed datagrams are skipped without crashing",
          "[two_phase_commit][udp]") {
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
  auto sent = ::sendto(raw_fd, junk, std::strlen(junk), 0,
                       reinterpret_cast<sockaddr*>(&coord_addr),
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

TEST_CASE("UDP: timer fires without incoming UDP",
          "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  // Protocol that sets a timer on start and finishes when the timer fires.
  struct TimerProto {
    bool fired = false;
    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment&) { fired = true; });
      return true;
    }
    // Timer fires asynchronously; no messages expected. The run() loop
    // will invoke the callback, then we need a way to signal completion.
    // We rely on a dummy message sent by the timer callback.
    bool receive(tpc::Environment&, const tpc::Message&) { return false; }
  };

  // We need a variant that sends itself a message from the timer callback
  // so run()'s dequeue unblocks.
  struct TimerSelfMsg {
    bool timer_fired = false;
    tpc::ParticipantId my_id;
    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment& e) {
        timer_fired = true;
        e.send(my_id, tpc::Prepare{});
      });
      return true;
    }
    bool receive(tpc::Environment&, const tpc::Message&) { return false; }
  };

  tpc::UdpEnvironment env(tpc::kCoordinator, pm);
  TimerSelfMsg proto{false, tpc::kCoordinator};
  env.run(proto);
  REQUIRE(proto.timer_fired);
}

TEST_CASE("UDP: canceled timer does not fire",
          "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  struct CancelProto {
    bool timer_fired = false;
    tpc::ParticipantId my_id;

    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment&) { timer_fired = true; });
      env.cancel_timer(1);
      // Set a second timer to end the protocol.
      env.set_timer(2, 30, [this](tpc::Environment& e) {
        e.send(my_id, tpc::Prepare{});
      });
      return true;
    }
    bool receive(tpc::Environment&, const tpc::Message&) { return false; }
  };

  tpc::UdpEnvironment env(tpc::kCoordinator, pm);
  CancelProto proto{false, tpc::kCoordinator};
  env.run(proto);
  REQUIRE_FALSE(proto.timer_fired);
}

TEST_CASE("UDP: replace same id fires only newest callback",
          "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  struct ReplaceProto {
    int which_fired = 0;
    tpc::ParticipantId my_id;

    bool start(tpc::Environment& env) {
      env.set_timer(1, 10, [this](tpc::Environment&) { which_fired = 1; });
      // Replace with new callback.
      env.set_timer(1, 10, [this](tpc::Environment& e) {
        which_fired = 2;
        e.send(my_id, tpc::Prepare{});
      });
      return true;
    }
    bool receive(tpc::Environment&, const tpc::Message&) { return false; }
  };

  tpc::UdpEnvironment env(tpc::kCoordinator, pm);
  ReplaceProto proto{0, tpc::kCoordinator};
  env.run(proto);
  REQUIRE(proto.which_fired == 2);
}

TEST_CASE("UDP: shutdown with pending timers exits cleanly",
          "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  struct ShutdownProto {
    tpc::ParticipantId my_id;
    bool start(tpc::Environment& env) {
      // Set a long timer that should never fire.
      env.set_timer(1, 60000, [](tpc::Environment&) {});
      // Send ourselves a message to end immediately.
      env.send(my_id, tpc::Prepare{});
      return true;
    }
    bool receive(tpc::Environment&, const tpc::Message&) { return false; }
  };

  tpc::UdpEnvironment env(tpc::kCoordinator, pm);
  ShutdownProto proto{tpc::kCoordinator};
  // Should return promptly despite the 60s pending timer.
  env.run(proto);
  // If we get here, shutdown was clean.
  REQUIRE(true);
}

TEST_CASE("UDP: participant locally aborts when decision timer fires",
          "[two_phase_commit][udp][timer]") {
  auto pm = make_localhost_port_map(1);

  tpc::UdpEnvironment coordinator_sender(tpc::kCoordinator, pm);
  tpc::UdpEnvironment participant_env(1, pm, tpc::Vote::Yes);
  tpc::Participant participant(1, /*decision_timeout_ms=*/10);

  coordinator_sender.send(1, tpc::Prepare{});
  participant_env.run(participant);

  REQUIRE(participant.outcome() == tpc::Decision::Abort);
}
