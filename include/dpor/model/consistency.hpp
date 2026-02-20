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
class AsyncConsistencyCheckerT {
 public:
  [[nodiscard]] ConsistencyResult check(const ExecutionGraphT<ValueT>&) const {
    return ConsistencyResult::failure(
        ConsistencyIssueCode::UnimplementedCheck,
        "Async consistency checking is not implemented yet.");
  }
};

using AsyncConsistencyChecker = AsyncConsistencyCheckerT<Value>;

}  // namespace dpor::model
