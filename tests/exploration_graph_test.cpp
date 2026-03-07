#include "dpor/model/exploration_graph.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <unordered_set>

namespace {
using namespace dpor::model;

void require_same_porf_reachability(ExplorationGraph& lhs, ExplorationGraph& rhs) {
  REQUIRE(lhs.event_count() == rhs.event_count());

  const bool lhs_has_cycle = lhs.has_causal_cycle();
  const bool rhs_has_cycle = rhs.has_causal_cycle();
  REQUIRE(lhs_has_cycle == rhs_has_cycle);
  if (lhs_has_cycle) {
    return;
  }

  for (ExplorationGraph::EventId from = 0; from < lhs.event_count(); ++from) {
    for (ExplorationGraph::EventId to = 0; to < lhs.event_count(); ++to) {
      REQUIRE(lhs.porf_contains(from, to) == rhs.porf_contains(from, to));
    }
  }
}
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

TEST_CASE("thread_trace includes bottom for non-blocking receives",
    "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto r = g.add_event(2, make_nonblocking_receive_label<Value>());
  g.set_reads_from_bottom(r);

  const auto trace = g.thread_trace(2);
  REQUIRE(trace.size() == 1);
  REQUIRE(trace[0].is_bottom());
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

TEST_CASE("restrict preserves bottom rf assignments for kept receives",
    "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto r = g.add_event(2, make_nonblocking_receive_label<Value>());
  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "x"}));
  g.set_reads_from_bottom(r);

  const auto restricted = g.restrict({r});
  REQUIRE(restricted.event_count() == 1);
  REQUIRE(restricted.reads_from().size() == 1);
  REQUIRE(restricted.reads_from().at(0).is_bottom());
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

TEST_CASE("with_bottom_rf returns copy with changed rf", "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_nonblocking_receive_label<Value>());

  g.set_reads_from(r, s);

  const auto g2 = g.with_bottom_rf(r);

  REQUIRE(g.reads_from().at(r).is_send());
  REQUIRE(g2.reads_from().at(r).is_bottom());
}

TEST_CASE("reads_from iteration preserves assigned receive ids and skips gaps",
    "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r1 = g.add_event(2, make_receive_label<Value>());
  static_cast<void>(g.add_event(3, SendLabel{.destination = 4, .value = "y"}));
  const auto r2 = g.add_event(4, make_nonblocking_receive_label<Value>());

  g.set_reads_from(r1, s1);
  g.set_reads_from_bottom(r2);

  std::vector<ExplorationGraph::EventId> receive_ids;
  for (const auto& [receive_id, source] : g.reads_from()) {
    receive_ids.push_back(receive_id);
    REQUIRE((source.is_send() || source.is_bottom()));
  }

  REQUIRE(receive_ids == std::vector<ExplorationGraph::EventId>{r1, r2});
  REQUIRE(g.reads_from().find(r1) != g.reads_from().end());
  REQUIRE(g.reads_from().find(2) == g.reads_from().end());
}

TEST_CASE("reads_from iterator dereference stays stable across increment",
    "[model][exploration_graph]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r1 = g.add_event(2, make_receive_label<Value>());
  static_cast<void>(g.add_event(3, SendLabel{.destination = 4, .value = "y"}));
  const auto r2 = g.add_event(4, make_nonblocking_receive_label<Value>());

  g.set_reads_from(r1, s1);
  g.set_reads_from_bottom(r2);

  auto it = g.reads_from().begin();
  const auto& first = *it;
  ++it;
  const auto& second = *it;

  REQUIRE(first.first == r1);
  REQUIRE(first.second == s1);
  REQUIRE(second.first == r2);
  REQUIRE(second.second.is_bottom());
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

// --- known acyclic metadata ---

TEST_CASE("known acyclicity is preserved on safe forward appends",
    "[model][exploration_graph][known_acyclic]") {
  ExplorationGraph g;
  REQUIRE(g.is_known_acyclic());

  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  REQUIRE(g.is_known_acyclic());

  const auto r = g.add_event(2, make_receive_label<Value>());
  REQUIRE(g.is_known_acyclic());
  g.set_reads_from(r, s);
  REQUIRE(g.is_known_acyclic());

  const auto nb = g.add_event(2, make_nonblocking_receive_label<Value>());
  REQUIRE(g.is_known_acyclic());
  g.set_reads_from_bottom(nb);
  REQUIRE(g.is_known_acyclic());
}

TEST_CASE("with_nd_value preserves known acyclicity metadata",
    "[model][exploration_graph][known_acyclic]") {
  ExplorationGraph g;
  const auto nd = g.add_event(
      1, NondeterministicChoiceLabel{.value = "old", .choices = {"old", "new"}});

  REQUIRE(g.is_known_acyclic());
  const auto g2 = g.with_nd_value(nd, "new");
  REQUIRE(g2.is_known_acyclic());
}

TEST_CASE("older fresh receives lose the append-only fast path once they stop being leaves",
    "[model][exploration_graph][known_acyclic]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());

  REQUIRE(g.is_known_acyclic());

  static_cast<void>(g.add_event(2, SendLabel{.destination = 1, .value = "y"}));
  REQUIRE(g.is_known_acyclic());

  g.set_reads_from(r, s);
  REQUIRE_FALSE(g.is_known_acyclic());
}

TEST_CASE("rewriting rf on a pre-existing receive clears known acyclicity",
    "[model][exploration_graph][known_acyclic]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());
  g.set_reads_from(r, s1);

  REQUIRE(g.is_known_acyclic());

  const auto s2 = g.add_event(1, SendLabel{.destination = 2, .value = "y"});
  REQUIRE(g.is_known_acyclic());

  g.set_reads_from(r, s2);
  REQUIRE_FALSE(g.is_known_acyclic());
}

TEST_CASE("restrict and rf-copy helpers clear known acyclicity",
    "[model][exploration_graph][known_acyclic]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());
  g.set_reads_from(r, s);

  REQUIRE(g.is_known_acyclic());

  const auto rewired = g.with_rf(r, s);
  REQUIRE_FALSE(rewired.is_known_acyclic());

  const auto bottomed = g.with_bottom_rf(r);
  REQUIRE_FALSE(bottomed.is_known_acyclic());

  const auto restricted = g.restrict({s, r});
  REQUIRE_FALSE(restricted.is_known_acyclic());
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

TEST_CASE(
    "cycle-only causal check avoids populating porf cache",
    "[model][exploration_graph][porf_cache]") {
  ExplorationGraph g;
  const auto r1 = g.add_event(1, make_receive_label<Value>());
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto r2 = g.add_event(2, make_receive_label<Value>());
  const auto s2 = g.add_event(2, SendLabel{.destination = 1, .value = "b"});

  g.set_reads_from(r1, s2);
  g.set_reads_from(r2, s1);

  REQUIRE_FALSE(g.has_porf_cache());
  REQUIRE(g.has_causal_cycle_without_cache());
  REQUIRE_FALSE(g.has_porf_cache());

  REQUIRE(g.has_causal_cycle());
  REQUIRE(g.has_porf_cache());
}

// --- PorfCache tests ---

TEST_CASE("multi-hop porf reachability via rf and po", "[model][exploration_graph][porf_cache]") {
  ExplorationGraph g;
  // T1: send -> T2
  const auto t1_send = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  // T2: recv, then send -> T3
  const auto t2_recv = g.add_event(2, make_receive_label<Value>());
  const auto t2_send = g.add_event(2, SendLabel{.destination = 3, .value = "b"});
  // T3: recv
  const auto t3_recv = g.add_event(3, make_receive_label<Value>());

  g.set_reads_from(t2_recv, t1_send);
  g.set_reads_from(t3_recv, t2_send);

  // t1_send -rf-> t2_recv -po-> t2_send -rf-> t3_recv
  REQUIRE(g.porf_contains(t1_send, t3_recv));
  REQUIRE(g.porf_contains(t1_send, t2_recv));
  REQUIRE(g.porf_contains(t1_send, t2_send));
  REQUIRE(g.porf_contains(t2_send, t3_recv));

  // Not reachable in reverse direction.
  REQUIRE_FALSE(g.porf_contains(t3_recv, t1_send));
  REQUIRE_FALSE(g.porf_contains(t2_recv, t1_send));

  // Self is not reachable (acyclic graph).
  REQUIRE_FALSE(g.porf_contains(t1_send, t1_send));
}

TEST_CASE("cycle detection via with_rf", "[model][exploration_graph][porf_cache]") {
  ExplorationGraph g;
  // T1: recv, send -> T2
  const auto r1 = g.add_event(1, make_receive_label<Value>());
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "a"});

  // T2: recv, send -> T1
  const auto r2 = g.add_event(2, make_receive_label<Value>());
  const auto s2 = g.add_event(2, SendLabel{.destination = 1, .value = "b"});

  // First: only one rf edge, no cycle.
  g.set_reads_from(r2, s1);
  REQUIRE_FALSE(g.has_causal_cycle());

  // with_rf adds the back edge creating a cycle.
  const auto cyclic = g.with_rf(r1, s2);
  REQUIRE(cyclic.has_causal_cycle());

  // Original is still acyclic.
  REQUIRE_FALSE(g.has_causal_cycle());
}

TEST_CASE("warm cached parent extends porf cache for fresh send append",
    "[model][exploration_graph][porf_cache][incremental]") {
  ExplorationGraph parent;
  const auto s1 = parent.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto r1 = parent.add_event(2, make_receive_label<Value>());
  parent.set_reads_from(r1, s1);

  REQUIRE(parent.porf_contains(s1, r1));
  REQUIRE(parent.has_porf_cache());

  auto child = parent;
  static_cast<void>(child.add_event(2, SendLabel{.destination = 3, .value = "b"}));
  REQUIRE(child.is_known_acyclic());
  REQUIRE_FALSE(child.has_porf_cache());

  ExplorationGraph reference;
  const auto ref_s1 = reference.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto ref_r1 = reference.add_event(2, make_receive_label<Value>());
  reference.set_reads_from(ref_r1, ref_s1);
  static_cast<void>(reference.add_event(2, SendLabel{.destination = 3, .value = "b"}));

  require_same_porf_reachability(child, reference);
}

TEST_CASE("warm cached parent extends porf cache for fresh receive rf append",
    "[model][exploration_graph][porf_cache][incremental]") {
  ExplorationGraph parent;
  const auto s1 = parent.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto s2 = parent.add_event(1, SendLabel{.destination = 2, .value = "b"});
  const auto r1 = parent.add_event(2, make_receive_label_from_values<Value>({"a"}));
  parent.set_reads_from(r1, s1);

  REQUIRE(parent.porf_contains(s1, r1));
  REQUIRE(parent.has_porf_cache());

  auto child = parent;
  const auto r2 = child.add_event(2, make_receive_label_from_values<Value>({"b"}));
  child.set_reads_from(r2, s2);
  REQUIRE(child.is_known_acyclic());
  REQUIRE_FALSE(child.has_porf_cache());

  ExplorationGraph reference;
  const auto ref_s1 = reference.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto ref_s2 = reference.add_event(1, SendLabel{.destination = 2, .value = "b"});
  const auto ref_r1 = reference.add_event(2, make_receive_label_from_values<Value>({"a"}));
  reference.set_reads_from(ref_r1, ref_s1);
  const auto ref_r2 = reference.add_event(2, make_receive_label_from_values<Value>({"b"}));
  reference.set_reads_from(ref_r2, ref_s2);

  require_same_porf_reachability(child, reference);
}

TEST_CASE("incremental porf extension widens clocks for first event on a new thread",
    "[model][exploration_graph][porf_cache][incremental]") {
  ExplorationGraph parent;
  const auto s1 = parent.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto r1 = parent.add_event(2, make_receive_label<Value>());
  parent.set_reads_from(r1, s1);

  REQUIRE(parent.porf_contains(s1, r1));
  REQUIRE(parent.has_porf_cache());

  auto child = parent;
  static_cast<void>(child.add_event(3, SendLabel{.destination = 1, .value = "c"}));
  REQUIRE(child.is_known_acyclic());
  REQUIRE_FALSE(child.has_porf_cache());

  ExplorationGraph reference;
  const auto ref_s1 = reference.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto ref_r1 = reference.add_event(2, make_receive_label<Value>());
  reference.set_reads_from(ref_r1, ref_s1);
  static_cast<void>(reference.add_event(3, SendLabel{.destination = 1, .value = "c"}));

  require_same_porf_reachability(child, reference);
}

TEST_CASE("cache invalidation on set_reads_from", "[model][exploration_graph][porf_cache]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto s2 = g.add_event(1, SendLabel{.destination = 2, .value = "y"});
  const auto r = g.add_event(2, make_receive_label<Value>());

  g.set_reads_from(r, s1);
  // s1 -rf-> r, so s1 reaches r.
  REQUIRE(g.porf_contains(s1, r));
  // s2 is po-after s1, so s1 -po-> s2, but s2 does NOT reach r.
  REQUIRE_FALSE(g.porf_contains(s2, r));

  // Change rf to point at s2 instead.
  g.set_reads_from(r, s2);
  // Now s2 -rf-> r.
  REQUIRE(g.porf_contains(s2, r));
  // s1 -po-> s2 -rf-> r, so s1 still reaches r.
  REQUIRE(g.porf_contains(s1, r));
}

TEST_CASE("unsafe rf rewrites do not reuse stale warm-parent reachability",
    "[model][exploration_graph][porf_cache][incremental]") {
  ExplorationGraph parent;
  const auto s1 = parent.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto s2 = parent.add_event(1, SendLabel{.destination = 2, .value = "b"});
  const auto r = parent.add_event(2, make_receive_label<Value>());
  parent.set_reads_from(r, s1);

  REQUIRE(parent.porf_contains(s1, r));
  REQUIRE(parent.has_porf_cache());

  const auto rewired = parent.with_rf(r, s2);
  REQUIRE_FALSE(rewired.is_known_acyclic());
  REQUIRE_FALSE(rewired.has_porf_cache());

  ExplorationGraph reference;
  const auto ref_s1 = reference.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto ref_s2 = reference.add_event(1, SendLabel{.destination = 2, .value = "b"});
  const auto ref_r = reference.add_event(2, make_receive_label<Value>());
  reference.set_reads_from(ref_r, ref_s2);

  auto rewired_copy = rewired;
  require_same_porf_reachability(rewired_copy, reference);
}

TEST_CASE("restrict preserves porf reachability on subset", "[model][exploration_graph][porf_cache]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());
  const auto s2 = g.add_event(2, SendLabel{.destination = 3, .value = "y"});
  g.set_reads_from(r, s);

  // Full graph: s -rf-> r -po-> s2.
  REQUIRE(g.porf_contains(s, s2));

  // Restrict to {s, r}: s2 is removed.
  std::unordered_set<ExplorationGraph::EventId> keep{s, r};
  const auto restricted = g.restrict(keep);
  REQUIRE(restricted.event_count() == 2);
  // In restricted graph: event 0 (was s) -rf-> event 1 (was r).
  REQUIRE(restricted.porf_contains(0, 1));
  REQUIRE_FALSE(restricted.porf_contains(1, 0));
}

TEST_CASE("with_rf invalidation produces distinct reachability", "[model][exploration_graph][porf_cache]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto s2 = g.add_event(1, SendLabel{.destination = 2, .value = "b"});
  const auto r = g.add_event(2, make_receive_label<Value>());

  g.set_reads_from(r, s1);
  // Original: s1 -rf-> r.
  REQUIRE(g.porf_contains(s1, r));

  // Copy with rf changed to s2.
  const auto copy = g.with_rf(r, s2);
  // Copy: s2 -rf-> r.
  REQUIRE(copy.porf_contains(s2, r));
  // s1 -po-> s2 -rf-> r in copy.
  REQUIRE(copy.porf_contains(s1, r));

  // Original is unchanged.
  REQUIRE(g.porf_contains(s1, r));
}

// --- Malformed reads-from regression tests ---

TEST_CASE(
    "porf_contains rejects reads-from edges whose target is not a receive",
    "[model][exploration_graph][porf_cache][regression]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto s2 = g.add_event(2, SendLabel{.destination = 1, .value = "b"});

  // Malformed edge: reads-from target must be a receive event.
  g.set_reads_from(s2, s1);

  // Baseline relation construction rejects this graph.
  REQUIRE_THROWS_AS(g.rf_relation(), std::invalid_argument);
  // porf_contains should surface the same malformed-rf error.
  REQUIRE_THROWS_AS(g.porf_contains(s1, s2), std::invalid_argument);
}

TEST_CASE(
    "porf_contains rejects reads-from edges with out-of-range endpoints",
    "[model][exploration_graph][porf_cache][regression]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());

  g.set_reads_from(r, s);
  // Malformed edge: invalid receive id.
  g.set_reads_from(999, s);

  // Baseline relation construction rejects this graph.
  REQUIRE_THROWS_AS(g.rf_relation(), std::invalid_argument);
  // porf_contains should surface the same malformed-rf error.
  REQUIRE_THROWS_AS(g.porf_contains(s, r), std::invalid_argument);
}

TEST_CASE(
    "has_causal_cycle rejects malformed reads-from edges",
    "[model][exploration_graph][porf_cache][regression]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});

  // Malformed edge: invalid receive id.
  g.set_reads_from(12345, s);

  // Baseline relation construction rejects this graph.
  REQUIRE_THROWS_AS(g.rf_relation(), std::invalid_argument);
  REQUIRE_FALSE(g.has_porf_cache());
  REQUIRE_THROWS_AS(g.has_causal_cycle_without_cache(), std::invalid_argument);
  REQUIRE_FALSE(g.has_porf_cache());
  // has_causal_cycle should surface the same malformed-rf error.
  REQUIRE_THROWS_AS(g.has_causal_cycle(), std::invalid_argument);
}

TEST_CASE(
    "porf_contains rejects invalid event ids",
    "[model][exploration_graph][porf_cache][regression]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto r = g.add_event(2, make_receive_label<Value>());
  g.set_reads_from(r, s);

  REQUIRE_THROWS_AS(g.porf_contains(s, 999), std::out_of_range);
  REQUIRE_THROWS_AS(g.porf_contains(999, r), std::out_of_range);
}

// --- thread_event_count ---

TEST_CASE("thread_event_count tracks events per thread", "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  REQUIRE(g.thread_event_count(1) == 0);
  REQUIRE(g.thread_event_count(99) == 0);

  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "a"}));
  REQUIRE(g.thread_event_count(1) == 1);
  REQUIRE(g.thread_event_count(2) == 0);

  static_cast<void>(g.add_event(2, make_receive_label<Value>()));
  REQUIRE(g.thread_event_count(1) == 1);
  REQUIRE(g.thread_event_count(2) == 1);

  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "b"}));
  REQUIRE(g.thread_event_count(1) == 2);
}

// --- thread_is_terminated ---

TEST_CASE("thread_is_terminated after sends", "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  REQUIRE_FALSE(g.thread_is_terminated(1));

  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "a"}));
  REQUIRE_FALSE(g.thread_is_terminated(1));
}

TEST_CASE("thread_is_terminated after block", "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "a"}));
  static_cast<void>(g.add_event(1, BlockLabel{}));
  REQUIRE(g.thread_is_terminated(1));
}

TEST_CASE("thread_is_terminated after error", "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  static_cast<void>(g.add_event(1, ErrorLabel{}));
  REQUIRE(g.thread_is_terminated(1));
}

TEST_CASE("thread_is_terminated after receives", "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  static_cast<void>(g.add_event(1, make_receive_label<Value>()));
  REQUIRE_FALSE(g.thread_is_terminated(1));
}

TEST_CASE("thread_is_terminated for nonexistent thread", "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "a"}));
  REQUIRE_FALSE(g.thread_is_terminated(99));
}

// --- last_event_id ---

TEST_CASE("last_event_id returns kNoSource for empty/nonexistent thread",
    "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  REQUIRE(g.last_event_id(1) == ExplorationGraph::kNoSource);
  REQUIRE(g.last_event_id(99) == ExplorationGraph::kNoSource);
}

TEST_CASE("last_event_id tracks most recent event", "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  const auto a = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  REQUIRE(g.last_event_id(1) == a);

  const auto b = g.add_event(1, SendLabel{.destination = 2, .value = "b"});
  REQUIRE(g.last_event_id(1) == b);

  // Other thread unaffected.
  REQUIRE(g.last_event_id(2) == ExplorationGraph::kNoSource);
}

// --- thread_trace after with_rf retargets ---

TEST_CASE("thread_trace after with_rf retargets a receive",
    "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto s2 = g.add_event(1, SendLabel{.destination = 2, .value = "y"});
  const auto r = g.add_event(2, make_receive_label<Value>());
  g.set_reads_from(r, s1);

  REQUIRE(g.thread_trace(2).size() == 1);
  REQUIRE(g.thread_trace(2)[0] == "x");

  const auto g2 = g.with_rf(r, s2);
  REQUIRE(g2.thread_trace(2).size() == 1);
  REQUIRE(g2.thread_trace(2)[0] == "y");

  // Original unchanged.
  REQUIRE(g.thread_trace(2)[0] == "x");
}

// --- thread_trace after restrict ---

TEST_CASE("thread_trace after restrict remaps correctly",
    "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  const auto s1 = g.add_event(1, SendLabel{.destination = 2, .value = "x"});
  const auto s2 = g.add_event(1, SendLabel{.destination = 2, .value = "y"});
  const auto r1 = g.add_event(2, make_receive_label<Value>());
  const auto r2 = g.add_event(2, make_receive_label<Value>());
  g.set_reads_from(r1, s1);
  g.set_reads_from(r2, s2);

  // Full trace for thread 2: ["x", "y"]
  REQUIRE(g.thread_trace(2).size() == 2);

  // Restrict to {s2, r2} — only the second send-receive pair.
  const auto restricted = g.restrict({s2, r2});
  REQUIRE(restricted.event_count() == 2);

  const auto trace = restricted.thread_trace(2);
  REQUIRE(trace.size() == 1);
  REQUIRE(trace[0] == "y");
}

// --- thread_event_count and thread_is_terminated after restrict ---

TEST_CASE("thread_event_count correct after restrict",
    "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  const auto a = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  static_cast<void>(g.add_event(1, SendLabel{.destination = 2, .value = "b"}));
  static_cast<void>(g.add_event(2, make_receive_label<Value>()));

  const auto restricted = g.restrict({a});
  REQUIRE(restricted.thread_event_count(1) == 1);
  REQUIRE(restricted.thread_event_count(2) == 0);
}

TEST_CASE("thread_is_terminated correct after restrict removes block",
    "[model][exploration_graph][thread_state]") {
  ExplorationGraph g;
  const auto s = g.add_event(1, SendLabel{.destination = 2, .value = "a"});
  const auto b = g.add_event(1, BlockLabel{});

  REQUIRE(g.thread_is_terminated(1));

  // Restrict to just the send — block removed.
  const auto restricted = g.restrict({s});
  REQUIRE_FALSE(restricted.thread_is_terminated(1));
  static_cast<void>(b);
}
