#pragma once

#include "dpor/model/execution_graph.hpp"

#include <string>
#include <utility>
#include <vector>

namespace dpor::model {

enum class ConsistencyIssueCode {
  InvalidEventReference,
  UnimplementedCheck,
};

struct ConsistencyIssue {
  ConsistencyIssueCode code{};
  std::string message{};
};

struct ConsistencyResult {
  std::vector<ConsistencyIssue> issues{};

  [[nodiscard]] static ConsistencyResult success() {
    return ConsistencyResult{};
  }

  [[nodiscard]] static ConsistencyResult failure(ConsistencyIssueCode code, std::string message) {
    ConsistencyResult result;
    result.issues.push_back(ConsistencyIssue{code, std::move(message)});
    return result;
  }

  [[nodiscard]] bool is_consistent() const noexcept {
    return issues.empty();
  }
};

template <typename ValueT>
class P2PConsistencyCheckerT {
 public:
  [[nodiscard]] ConsistencyResult check(const ExecutionGraphT<ValueT>&) const {
    return ConsistencyResult::failure(
        ConsistencyIssueCode::UnimplementedCheck,
        "P2P consistency checking is not implemented yet.");
  }
};

using P2PConsistencyChecker = P2PConsistencyCheckerT<Value>;

}  // namespace dpor::model
