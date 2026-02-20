#include "dpor/api/session.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("session keeps provided config values", "[session]") {
  const dpor::api::Session session{{"unit-test", 42}};

  REQUIRE(session.config().name == "unit-test");
  REQUIRE(session.config().max_depth == 42);
}

TEST_CASE("session description includes configured name", "[session]") {
  const dpor::api::Session session{{"unit-test", 42}};
  const std::string desc = session.describe();

  REQUIRE(desc.find("unit-test") != std::string::npos);
}
