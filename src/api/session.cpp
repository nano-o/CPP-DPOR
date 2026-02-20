#include "dpor/api/session.hpp"

#include <sstream>

namespace dpor::api {

Session::Session(SessionConfig config) : config_(std::move(config)) {}

const SessionConfig& Session::config() const noexcept {
  return config_;
}

std::string Session::describe() const {
  std::ostringstream out;
  out << "dpor::api::Session{name=\"" << config_.name << "\", max_depth=" << config_.max_depth << "}";
  return out.str();
}

}  // namespace dpor::api
