#pragma once

#include "oracle_core.hpp"
#include <catch2/catch_test_macros.hpp>

namespace dpor::test_support {

template <typename ValueT>
inline void require_dpor_matches_oracle(const algo::ProgramT<ValueT>& program,
                                        const std::string& description,
                                        const model::CommunicationModel communication_model =
                                            model::CommunicationModel::Async) {
  const auto comparison = compare_dpor_with_oracle(program, communication_model);

  INFO("oracle program: " << description);
  INFO("oracle signatures: " << comparison.oracle_signatures.size());
  INFO("dpor full_executions_explored: " << comparison.result.full_executions_explored);
  INFO("dpor error_executions_explored: " << comparison.result.error_executions_explored);
  INFO("dpor depth_limit_executions_explored: "
       << comparison.result.depth_limit_executions_explored);
  INFO("dpor unique signatures: " << comparison.dpor_unique.size());
  INFO("missing signatures: " << comparison.missing_from_dpor.size());
  INFO("unexpected signatures: " << comparison.unexpected_in_dpor.size());

  REQUIRE_FALSE(comparison.found_inconsistent_graph);
  REQUIRE(comparison.result.kind == algo::VerifyResultKind::AllExplored);
  REQUIRE(comparison.dpor_unique.size() == comparison.dpor_observed.size());
  REQUIRE(comparison.dpor_unique == comparison.oracle_signatures);
}

}  // namespace dpor::test_support
