#pragma once

#include "../protocol.hpp"
#include "bridge.hpp"

#include <functional>
#include <initializer_list>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tpc_sim {

struct ScenarioThreadTerminated {};
struct StepBoundaryReached {};
struct TimeoutSimulationFailure : std::logic_error {
  using std::logic_error::logic_error;
};

[[nodiscard]] inline EventLabel make_protocol_error_label(std::string message) {
  return EventLabel{dpor::model::ErrorLabel{.message = std::move(message)}};
}

template <typename ResultT, typename Fn>
[[nodiscard]] std::optional<EventLabel> invoke_protocol_step(ResultT& out, Fn&& fn) {
  try {
    out = std::forward<Fn>(fn)();
    return std::nullopt;
  } catch (const ScenarioThreadTerminated&) {
    throw;
  } catch (const StepBoundaryReached&) {
    throw;
  } catch (const TimeoutSimulationFailure&) {
    throw;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& ex) {
    return make_protocol_error_label(ex.what());
  } catch (...) {
    return make_protocol_error_label("uncaught non-standard exception");
  }
}

struct SimReceiveResult {
  enum class Kind {
    ReplayedMessage,
    ReplayedTimerFire,
    CapturedReceive,
  };

  Kind kind;
  std::optional<tpc::Message> message{};
  // Used only for ReplayedTimerFire: true means the timer callback kept the
  // protocol waiting for input, false means it completed the protocol.
  bool needs_message{true};

  [[nodiscard]] static SimReceiveResult replayed_message(tpc::Message msg) {
    return SimReceiveResult{
        .kind = Kind::ReplayedMessage,
        .message = std::move(msg),
    };
  }

  [[nodiscard]] static SimReceiveResult replayed_timer_fire(bool keep_waiting) {
    return SimReceiveResult{
        .kind = Kind::ReplayedTimerFire,
        .needs_message = keep_waiting,
    };
  }

  [[nodiscard]] static SimReceiveResult captured_receive() {
    return SimReceiveResult{
        .kind = Kind::CapturedReceive,
    };
  }
};

class ReplayCore {
 public:
  ReplayCore(std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map, std::size_t target_io,
             const ThreadTrace& trace, std::size_t trace_offset)
      : id_map_(std::move(id_map)),
        target_io_(target_io),
        trace_(trace),
        trace_offset_(trace_offset) {}

  void capture_send(tpc::ParticipantId destination, SimValue value) {
    auto current = io_count_++;
    if (current < target_io_) {
      return;
    }

    result_ = SendLabel{
        .destination = id_map_(destination),
        .value = value,
    };
    throw StepBoundaryReached{};
  }

  [[nodiscard]] SimValue replay_choice_or_capture(std::initializer_list<SimValue> choices) {
    auto current = io_count_++;

    if (current < target_io_) {
      auto idx = trace_offset_ + trace_consume_count_++;
      return trace_value(idx);
    }

    result_ = NondeterministicChoiceLabel{
        .value = {},
        .choices = std::vector<SimValue>(choices),
    };
    throw StepBoundaryReached{};
  }

  [[nodiscard]] SimReceiveResult receive_step(tpc::Environment& env) {
    auto current = io_count_++;

    if (current < target_io_) {
      auto idx = trace_offset_ + trace_consume_count_++;
      const auto& observed = trace_entry(idx);
      if (observed.is_bottom()) {
        return SimReceiveResult::replayed_timer_fire(fire_active_timer(env));
      }
      return SimReceiveResult::replayed_message(decode_sim_message(trace_value(idx)));
    }

    if (has_active_timer()) {
      result_ = dpor::model::make_nonblocking_receive_label<SimValue>();
    } else {
      result_ = dpor::model::make_receive_label<SimValue>();
    }
    return SimReceiveResult::captured_receive();
  }

  void set_timer(tpc::TimerId id, tpc::TimerCallback callback) {
    // Matching UdpEnvironment, setting the same timer id refreshes/replaces it.
    // A different id would mean multiple simultaneously-active timers, which
    // this simplified adapter does not encode in bottom observations.
    if (active_timer_.has_value() && active_timer_->id != id) {
      throw TimeoutSimulationFailure("SimEnvironment supports at most one active timer per thread");
    }
    active_timer_ = ActiveTimer{
        .id = id,
        .callback = std::move(callback),
    };
  }

  void cancel_timer(tpc::TimerId id) {
    if (active_timer_.has_value() && active_timer_->id == id) {
      active_timer_.reset();
    }
  }

  [[nodiscard]] std::optional<EventLabel> result() const { return result_; }

 private:
  struct ActiveTimer {
    tpc::TimerId id;
    tpc::TimerCallback callback;
  };

  [[nodiscard]] bool has_active_timer() const noexcept { return active_timer_.has_value(); }

  [[nodiscard]] const ThreadTrace::value_type& trace_entry(std::size_t idx) const {
    if (idx >= trace_.size()) {
      throw TimeoutSimulationFailure("trace shorter than expected for simulated replay");
    }
    return trace_[idx];
  }

  [[nodiscard]] const SimValue& trace_value(std::size_t idx) const {
    const auto& observed = trace_entry(idx);
    if (observed.is_bottom()) {
      throw TimeoutSimulationFailure("trace requested a concrete value but observed bottom");
    }
    return observed.value();
  }

  [[nodiscard]] tpc::Message decode_sim_message(SimValue value) const {
    try {
      return decode_message(value);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& ex) {
      throw TimeoutSimulationFailure(ex.what());
    }
  }

  [[nodiscard]] bool fire_active_timer(tpc::Environment& env) {
    if (!active_timer_.has_value()) {
      throw TimeoutSimulationFailure("trace requested timer firing but no timer is active");
    }
    auto callback = std::move(active_timer_->callback);
    active_timer_.reset();
    bool keep_waiting = false;
    if (const auto error = invoke_protocol_step(keep_waiting, [&]() { return callback(env); });
        error.has_value()) {
      result_ = *error;
      throw StepBoundaryReached{};
    }
    return keep_waiting;
  }

  std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map_;
  std::size_t target_io_;
  const ThreadTrace& trace_;
  std::size_t trace_offset_;

  std::size_t io_count_{0};
  std::size_t trace_consume_count_{0};
  std::optional<EventLabel> result_;
  std::optional<ActiveTimer> active_timer_;
};

template <typename ProtocolObj, typename SimEnvironmentT>
std::optional<EventLabel> run_and_capture(ProtocolObj& obj, SimEnvironmentT& env) {
  try {
    bool needs_message = false;
    if (const auto error = invoke_protocol_step(needs_message, [&]() { return obj.start(env); });
        error.has_value()) {
      return error;
    }
    while (needs_message) {
      auto receive_step = env.sim_receive();
      if (receive_step.kind == SimReceiveResult::Kind::CapturedReceive) {
        return env.result();
      }
      if (receive_step.kind == SimReceiveResult::Kind::ReplayedTimerFire) {
        needs_message = receive_step.needs_message;
        continue;
      }
      if (const auto error = invoke_protocol_step(
              needs_message, [&]() { return obj.receive(env, *receive_step.message); });
          error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  } catch (const ScenarioThreadTerminated&) {
    return std::nullopt;
  } catch (const StepBoundaryReached&) {
    return env.result();
  }
}

}  // namespace tpc_sim
