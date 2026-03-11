#include "dpor/algo/dpor.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include "simulation.hpp"
#include "udp_network.hpp"
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <iostream>
#include <optional>
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

// ---------------------------------------------------------------------------
// Trace helpers
// ---------------------------------------------------------------------------

// Participant trace layout (program order):
//   [0] = received Prepare (serialized)
//   [1] = vote ND choice ("YES" / "NO")
//   [2] = received DecisionMsg (serialized) -- only if coordinator didn't crash
static std::string get_participant_vote(const model::ExplorationGraph& graph,
                                        tpc::ParticipantId pid) {
  auto trace = graph.thread_trace(participant_to_thread(pid));
  if (trace.size() >= 2) {
    return trace[1].value();
  }
  return {};
}

static std::optional<std::string> get_participant_decision(const model::ExplorationGraph& graph,
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
  auto trace = graph.thread_trace(participant_to_thread(tpc::kCoordinator));
  if (trace.size() > num_participants) {
    return trace[num_participants] == "crash";
  }
  return false;
}

// Dump the global interleaving of an execution to stderr.
static void dump_global_trace(const model::ExplorationGraph& graph) {
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
// Simulation adapter tests
// ---------------------------------------------------------------------------

TEST_CASE("SimEnvironment turns protocol exceptions into error events",
          "[two_phase_commit][simulation]") {
  struct ThrowOnReceive {
    static bool start(tpc::Environment& /*env*/) { return true; }

    static bool receive(tpc::Environment& /*env*/, const tpc::Message& /*msg*/) {
      throw std::logic_error("protocol bug");
    }
  };

  ThrowOnReceive protocol;
  ThreadTrace trace{model::ObservedValue{"PREPARE"}};
  SimEnvironment env(participant_to_thread, /*target_io=*/1, trace,
                     /*trace_offset=*/0);

  const auto label = run_and_capture(protocol, env);
  REQUIRE(label.has_value());
  REQUIRE(std::holds_alternative<model::ErrorLabel>(*label));
  REQUIRE(std::get<model::ErrorLabel>(*label).message == "protocol bug");
}

// ---------------------------------------------------------------------------
// DPOR tests
// ---------------------------------------------------------------------------

TEST_CASE("2PC basic exploration with 2 participants", "[two_phase_commit]") {
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

TEST_CASE("2PC agreement invariant: all decided participants agree", "[two_phase_commit]") {
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

TEST_CASE("2PC validity invariant: Commit implies all voted Yes", "[two_phase_commit]") {
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

TEST_CASE("2PC without crashes explores 16 executions for 2 participants", "[two_phase_commit]") {
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

TEST_CASE("2PC protocol bug surfaces as verification failure", "[two_phase_commit]") {
  auto prog = make_two_phase_commit_program(2, /*inject_crash=*/false,
                                            /*bug_on_p1_no=*/true);

  bool saw_error_execution = false;
  algo::DporConfig config;
  config.program = std::move(prog);
  config.on_execution = [&](const model::ExplorationGraph& graph) {
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
  REQUIRE(::bind(fd,
                 reinterpret_cast<sockaddr*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                     &addr),
                 sizeof(addr)) ==
          0);

  socklen_t len = sizeof(addr);
  REQUIRE(::getsockname(
              fd,
              reinterpret_cast<sockaddr*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                  &addr),
              &len) ==
          0);
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
