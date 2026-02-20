#include "dpor/model/relation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <vector>

namespace {

std::vector<dpor::model::NodeId> collect_successors(
    const auto& relation,
    const dpor::model::NodeId from) {
  std::vector<dpor::model::NodeId> successors;
  relation.for_each_successor(from, [&](const dpor::model::NodeId to) {
    successors.push_back(to);
  });
  return successors;
}

}  // namespace

static_assert(dpor::model::Relation<dpor::model::ExplicitRelation>);
static_assert(dpor::model::Relation<dpor::model::ProgramOrderRelation>);

TEST_CASE("program order relation derives edges from thread sequences", "[model][relation][po]") {
  const dpor::model::ProgramOrderRelation po{
      6,
      {
          {0, 2, 4},
          {1, 3, 5},
      },
  };

  REQUIRE(po.contains(0, 2));
  REQUIRE(po.contains(0, 4));
  REQUIRE(po.contains(2, 4));
  REQUIRE_FALSE(po.contains(4, 2));
  REQUIRE_FALSE(po.contains(0, 3));

  REQUIRE(collect_successors(po, 0) == std::vector<dpor::model::NodeId>{2, 4});
  REQUIRE(collect_successors(po, 5).empty());
}

TEST_CASE("program order relation validates malformed thread assignments", "[model][relation][po]") {
  dpor::model::ProgramOrderRelation po{3};

  REQUIRE_THROWS_AS(
      po.set_thread_events({
          {0, 1},
          {1, 2},
      }),
      std::invalid_argument);
}

TEST_CASE("explicit relation stores and queries direct edges", "[model][relation][explicit]") {
  dpor::model::ExplicitRelation relation{4};
  relation.add_edge(0, 1);
  relation.add_edge(0, 2);
  relation.add_edge(0, 2);  // duplicate should not be stored twice

  REQUIRE(relation.contains(0, 1));
  REQUIRE(relation.contains(0, 2));
  REQUIRE_FALSE(relation.contains(1, 0));
  REQUIRE(collect_successors(relation, 0) == std::vector<dpor::model::NodeId>{1, 2});
}

TEST_CASE("compose relation computes relational composition", "[model][relation][compose]") {
  dpor::model::ExplicitRelation left{6};
  left.add_edge(0, 1);
  left.add_edge(0, 2);
  left.add_edge(1, 3);
  left.add_edge(2, 3);

  dpor::model::ExplicitRelation right{6};
  right.add_edge(1, 4);
  right.add_edge(2, 4);
  right.add_edge(3, 5);

  const auto composed = dpor::model::compose(left, right);

  REQUIRE(composed.contains(0, 4));
  REQUIRE(composed.contains(1, 5));
  REQUIRE(composed.contains(2, 5));
  REQUIRE_FALSE(composed.contains(0, 5));
  REQUIRE(collect_successors(composed, 0) == std::vector<dpor::model::NodeId>{4});
}

TEST_CASE("transitive closure relation computes reachability", "[model][relation][closure]") {
  dpor::model::ExplicitRelation base{5};
  base.add_edge(0, 1);
  base.add_edge(1, 2);
  base.add_edge(2, 1);  // cycle
  base.add_edge(2, 3);

  const auto closure = dpor::model::transitive_closure(base);

  REQUIRE(closure.contains(0, 1));
  REQUIRE(closure.contains(0, 2));
  REQUIRE(closure.contains(0, 3));
  REQUIRE(closure.contains(1, 1));  // cycle => reachable from itself via non-empty path
  REQUIRE_FALSE(closure.contains(3, 0));

  REQUIRE(collect_successors(closure, 0) == std::vector<dpor::model::NodeId>{1, 2, 3});
}
