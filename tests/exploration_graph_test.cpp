#include "dpor/model/exploration_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_set>

namespace {
using namespace dpor::model;
}  // namespace

// --- Insertion order tracking ---

TEST_CASE("insertion order tracks add_event calls", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto a = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto b = g.add_event(2, SendLabel{.destination = 1, .value = "b"});
  const auto c = g.add_event(1, SendLabel{.destination = 2, .value = "c"});

  const auto& order = g.insertion_order();
  REQUIRE(order.size() == 3);
  REQUIRE(order[0] == a);
  REQUIRE(order[1] == b);
  REQUIRE(order[2] == c);
}

TEST_CASE("inserted_before_or_equal checks insertion order", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto a = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto b = g.add_event(2, SendLabel{.destination = 1, .value = "b"});

  REQUIRE(g.inserted_before_or_equal(a, b));
  REQUIRE(g.inserted_before_or_equal(a, a));
  REQUIRE_FALSE(g.inserted_before_or_equal(b, a));
}

// --- thread_trace ---

TEST_CASE("thread_trace extracts values from receives via rf", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto s2 = g.add_event(1, SendLabel{.destination = 2, .value = "y"});
  const auto r1 = g.add_event(2, make_receive_label<Value>());
  const auto r2 = g.add_event(2, make_receive_label<Value>());

  g.set_reads_from(r1, s1);
  g.set_reads_from(r2, s2);

  const auto trace = g.thread_trace(2);
  REQUIRE(trace.size() == 2);
  REQUIRE(trace[0] == "x");
  REQUIRE(trace[1] == "y");
}

TEST_CASE("thread_trace includes ND choice values", "[model][exploration_graph]") {
  ExplorationGraph g;
  static_cast<void>(g.add_event(1, NondeterministicChoiceLabel{.value = "choice_a"}));
  static_cast<void>(g.add_event(1, NondeterministicChoiceLabel{.value = "choice_b"}));

  const auto trace = g.thread_trace(1);
  REQUIRE(trace.size() == 2);
  REQUIRE(trace[0] == "choice_a");
  REQUIRE(trace[1] == "choice_b");
}

TEST_CASE("thread_trace returns empty for thread with only sends", "[model][exploration_graph]") {
  ExplorationGraph g;
  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "x"}));

  const auto trace = g.thread_trace(1);
  REQUIRE(trace.empty());
}

TEST_CASE("thread_trace returns empty for nonexistent thread", "[model][exploration_graph]") {
  ExplorationGraph g;
  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "x"}));

  const auto trace = g.thread_trace(99);
  REQUIRE(trace.empty());
}

// --- restrict ---

TEST_CASE("restrict keeps only specified events", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto a = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto b = g.add_event(2, SendLabel{.destination = 1, .value = "b"});
  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "c"}));

  const auto restricted = g.restrict({a, b});
  REQUIRE(restricted.event_count() == 2);
}

TEST_CASE("restrict preserves rf edges between kept events", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());
  static_cast<void>(g.add_event(3, SendLabel{.destination = 1, .value = "y"}));

  g.set_reads_from(r, s);

  const auto restricted = g.restrict({s, r});
  REQUIRE(restricted.event_count() == 2);
  // The rf edge should be preserved (with remapped IDs).
  REQUIRE(restricted.reads_from().size() == 1);
}

TEST_CASE("restrict drops rf edges when one endpoint is removed", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());

  g.set_reads_from(r, s);

  // Keep only the receive, not the send.
  const auto restricted = g.restrict({r});
  REQUIRE(restricted.event_count() == 1);
  REQUIRE(restricted.reads_from().empty());
}

TEST_CASE("restrict preserves relative insertion order", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto a = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  static_cast<void>(g.add_event(2, SendLabel{.destination = 1, .value = "b"}));
  const auto c = g.add_event(1, SendLabel{.destination = 2, .value = "c"});

  const auto restricted = g.restrict({a, c});
  REQUIRE(restricted.event_count() == 2);
  const auto& order = restricted.insertion_order();
  REQUIRE(order.size() == 2);
  // Events should be in the order they were originally inserted.
  // After remapping, they'll be 0 and 1.
  REQUIRE(order[0] == 0);
  REQUIRE(order[1] == 1);
}

// --- with_rf ---

TEST_CASE("with_rf returns copy with changed rf", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto s2 = g.add_event(1, SendLabel{.destination = 2, .value = "y"});
  const auto r = g.add_event(2, make_receive_label<Value>());

  g.set_reads_from(r, s1);

  const auto g2 = g.with_rf(r, s2);

  // Original unchanged.
  REQUIRE(g.reads_from().at(r) == s1);
  // Copy has new rf.
  REQUIRE(g2.reads_from().at(r) == s2);
}

// --- with_nd_value ---

TEST_CASE("with_nd_value returns copy with changed ND value", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto nd = g.add_event(
      1, NondeterministicChoiceLabel{.value = "old", .choices = {"old", "new"}});

  const auto g2 = g.with_nd_value(nd, "new");

  // Original unchanged.
  const auto* orig_nd = as_nondeterministic_choice(g.event(nd));
  REQUIRE(orig_nd != nullptr);
  REQUIRE(orig_nd->value == "old");

  // Copy has new value.
  const auto* new_nd = as_nondeterministic_choice(g2.event(nd));
  REQUIRE(new_nd != nullptr);
  REQUIRE(new_nd->value == "new");
}

// --- porf_contains ---

TEST_CASE("porf_contains detects po reachability", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto a = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto b = g.add_event(1, SendLabel{.destination = 2, .value = "b"});

  REQUIRE(g.porf_contains(a, b));
  REQUIRE_FALSE(g.porf_contains(b, a));
}

TEST_CASE("porf_contains detects rf reachability", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());

  g.set_reads_from(r, s);

  REQUIRE(g.porf_contains(s, r));
  REQUIRE_FALSE(g.porf_contains(r, s));
}

TEST_CASE("porf_contains detects transitive po-rf chain", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r2 = g.add_event(2, make_receive_label<Value>());
  const auto s2 = g.add_event(2, SendLabel{.destination = 3, .value = "y"});

  g.set_reads_from(r2, s1);

  // s1 -rf-> r2 -po-> s2
  REQUIRE(g.porf_contains(s1, s2));
}

// --- receives_in_destination ---

TEST_CASE("receives_in_destination returns receives in target thread", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r1 = g.add_event(2, make_receive_label<Value>());
  const auto r2 = g.add_event(2, make_receive_label<Value>());
  static_cast<void>(g.add_event(3, make_receive_label<Value>()));

  const auto receives = g.receives_in_destination(s);
  REQUIRE(receives.size() == 2);
  REQUIRE(receives[0] == r1);
  REQUIRE(receives[1] == r2);
}

// --- has_causal_cycle ---

TEST_CASE("empty graph has no causal cycle", "[model][exploration_graph]") {
  ExplorationGraph g;
  REQUIRE_FALSE(g.has_causal_cycle());
}

TEST_CASE("acyclic send-receive has no causal cycle", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());
  g.set_reads_from(r, s);

  REQUIRE_FALSE(g.has_causal_cycle());
}

TEST_CASE("two-thread cycle detected as causal cycle", "[model][exploration_graph]") {
  ExplorationGraph g;
  // Thread 1: recv then send
  const auto r1 = g.add_event(1, make_receive_label<Value>());
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "a"});

  // Thread 2: recv then send
  const auto r2 = g.add_event(2, make_receive_label<Value>());
  const auto s2 = g.add_event(2, SendLabel{.destination = 1, .value = "b"});

  g.set_reads_from(r1, s2);
  g.set_reads_from(r2, s1);

  // r1 -po-> s1 -rf-> r2 -po-> s2 -rf-> r1: cycle
  REQUIRE(g.has_causal_cycle());
}
