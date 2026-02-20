#pragma once

#include <cstddef>
#include <string>

namespace dpor::api {

struct SessionConfig {
  std::string name{"default"};
  std::size_t max_depth{1000};
};

class Session {
 public:
  explicit Session(SessionConfig config);

  [[nodiscard]] const SessionConfig& config() const noexcept;
  [[nodiscard]] std::string describe() const;

 private:
  SessionConfig config_;
};

}  // namespace dpor::api
