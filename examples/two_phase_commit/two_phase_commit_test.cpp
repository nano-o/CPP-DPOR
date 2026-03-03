#include "simulation.hpp"
#include "udp_network.hpp"

#include "dpor/algo/dpor.hpp"
#include "dpor/model/event.hpp"
#include "dpor/model/exploration_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace dpor;
using namespace tpc_sim;

// Helper: extract the nondeterministic choice value (trace[0]) for a thread.
static std::string get_nd_choice(const model::ExplorationGraph& graph,
                                 model::ThreadId tid) {
  auto trace = graph.thread_trace(tid);
  if (trace.empty()) {
    return {};
  }
  return trace[0];
}

// Helper: check if the coordinator crashed in this execution.
static bool coordinator_crashed(const model::ExplorationGraph& graph) {
  return get_nd_choice(graph, participant_to_thread(tpc::kCoordinator)) ==
         "crash";
}

// Helper: extract the decision the coordinator would have reached.
// The coordinator's decision can be inferred from the votes:
// Commit iff all participants voted Yes.
// But in the simulation, the coordinator sends DecisionMsg which is visible
// in the participant traces.  We look at participant trace values instead.
static std::optional<std::string> get_participant_decision(
    const model::ExplorationGraph& graph,
    tpc::ParticipantId pid) {
  auto trace = graph.thread_trace(participant_to_thread(pid));
  // trace[0] = vote choice ("YES"/"NO")
  // trace[1] = received Prepare (serialized)
  // trace[2] = received DecisionMsg (serialized) -- if present
  if (trace.size() >= 3) {
    return trace[2];
  }
  return std::nullopt;
}

// Helper: get the vote for a participant.
static std::string get_participant_vote(
    const model::ExplorationGraph& graph,
    tpc::ParticipantId pid) {
  return get_nd_choice(graph, participant_to_thread(pid));
}

TEST_CASE("2PC basic exploration with 2 participants",
          "[two_phase_commit]") {
  auto prog = make_two_phase_commit_program(2);

  algo::DporConfig config;
  config.program = std::move(prog);

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  // There should be multiple executions explored (exact count depends on
  // interleaving of votes and message delivery).
  REQUIRE(result.executions_explored > 0);
}

TEST_CASE("2PC agreement invariant: all decided participants agree",
          "[two_phase_commit]") {
  constexpr std::size_t kNumParticipants = 2;
  auto prog = make_two_phase_commit_program(kNumParticipants);

  bool invariant_violated = false;

  algo::DporConfig config;
  config.program = std::move(prog);
  config.on_execution = [&](const model::ExplorationGraph& graph) {
    if (coordinator_crashed(graph)) {
      return;  // Skip crash executions for agreement check.
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
    if (coordinator_crashed(graph)) {
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
    if (!coordinator_crashed(graph)) {
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

TEST_CASE("2PC scales to 3 participants", "[two_phase_commit]") {
  auto prog = make_two_phase_commit_program(3);

  algo::DporConfig config;
  config.program = std::move(prog);

  const auto result = algo::verify(config);
  REQUIRE(result.kind == algo::VerifyResultKind::AllExecutionsExplored);
  REQUIRE(result.executions_explored > 0);
}

// ---------------------------------------------------------------------------
// UDP Network tests
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

  tpc::UdpNetwork sender(tpc::kCoordinator, pm);
  tpc::UdpNetwork receiver(1, pm);

  tpc::Message sent = tpc::Prepare{};
  sender.send(1, sent);

  tpc::Message got = receiver.receive();
  REQUIRE(std::holds_alternative<tpc::Prepare>(got));
}

TEST_CASE("UDP: send and receive VoteMsg preserves fields",
          "[two_phase_commit][udp]") {
  auto pm = make_localhost_port_map(1);

  tpc::UdpNetwork participant_net(1, pm);
  tpc::UdpNetwork coordinator_net(tpc::kCoordinator, pm);

  participant_net.send(tpc::kCoordinator,
                       tpc::VoteMsg{1, tpc::Vote::Yes});

  auto got = coordinator_net.receive();
  const auto* vote = std::get_if<tpc::VoteMsg>(&got);
  REQUIRE(vote != nullptr);
  REQUIRE(vote->from == 1);
  REQUIRE(vote->vote == tpc::Vote::Yes);
}

TEST_CASE("UDP: full 2PC protocol run with all-Yes votes commits",
          "[two_phase_commit][udp]") {
  constexpr std::size_t kN = 2;
  auto pm = make_localhost_port_map(kN);

  tpc::Coordinator coord(kN);
  tpc::Participant p1(1, tpc::Vote::Yes);
  tpc::Participant p2(2, tpc::Vote::Yes);

  // Create all networks on the main thread so every socket is bound
  // before any thread starts sending.
  tpc::UdpNetwork net_coord(tpc::kCoordinator, pm);
  tpc::UdpNetwork net_p1(1, pm);
  tpc::UdpNetwork net_p2(2, pm);

  std::thread t_coord([&] { coord.run(net_coord); });
  std::thread t_p1([&] { p1.run(net_p1); });
  std::thread t_p2([&] { p2.run(net_p2); });

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
  tpc::Participant p1(1, tpc::Vote::Yes);
  tpc::Participant p2(2, tpc::Vote::No);

  tpc::UdpNetwork net_coord(tpc::kCoordinator, pm);
  tpc::UdpNetwork net_p1(1, pm);
  tpc::UdpNetwork net_p2(2, pm);

  std::thread t_coord([&] { coord.run(net_coord); });
  std::thread t_p1([&] { p1.run(net_p1); });
  std::thread t_p2([&] { p2.run(net_p2); });

  t_coord.join();
  t_p1.join();
  t_p2.join();

  REQUIRE(coord.decision() == tpc::Decision::Abort);
  REQUIRE(p1.outcome() == tpc::Decision::Abort);
  REQUIRE(p2.outcome() == tpc::Decision::Abort);
}
