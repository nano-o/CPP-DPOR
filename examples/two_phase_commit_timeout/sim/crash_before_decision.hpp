#pragma once

#include "../protocol.hpp"
#include "bridge.hpp"

#include <functional>
#include <initializer_list>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tpc_sim::crash_before_decision {

struct Options {
  std::size_t num_participants;
  bool bug_on_p1_no{false};
  bool inject_crash{true};
};

namespace detail {

struct ScenarioThreadTerminated {};
struct StepBoundaryReached {};
struct SimulationFailure : std::logic_error {
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
  } catch (const SimulationFailure&) {
    throw;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& ex) {
    return make_protocol_error_label(ex.what());
  } catch (...) {
    return make_protocol_error_label("uncaught non-standard exception");
  }
}

template <typename T, typename... Args>
[[nodiscard]] std::optional<EventLabel> try_emplace_protocol_object(std::optional<T>& out,
                                                                    Args&&... args) {
  try {
    out.emplace(std::forward<Args>(args)...);
    return std::nullopt;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& ex) {
    return make_protocol_error_label(ex.what());
  } catch (...) {
    return make_protocol_error_label("uncaught non-standard exception");
  }
}

struct ReceiveResult {
  enum class Kind {
    ReplayedMessage,
    ReplayedTimerFire,
    CapturedReceive,
  };

  Kind kind;
  std::optional<tpc::Message> message{};
  bool needs_message{true};

  [[nodiscard]] static ReceiveResult replayed_message(tpc::Message msg) {
    return ReceiveResult{
        .kind = Kind::ReplayedMessage,
        .message = std::move(msg),
    };
  }

  [[nodiscard]] static ReceiveResult replayed_timer_fire(bool keep_waiting) {
    return ReceiveResult{
        .kind = Kind::ReplayedTimerFire,
        .needs_message = keep_waiting,
    };
  }

  [[nodiscard]] static ReceiveResult captured_receive() {
    return ReceiveResult{
        .kind = Kind::CapturedReceive,
    };
  }
};

class ReplayState {
 public:
  ReplayState(std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map,
              std::size_t target_io, const ThreadTrace& trace, std::size_t trace_offset)
      : id_map_(std::move(id_map)),
        target_io_(target_io),
        trace_(trace),
        trace_offset_(trace_offset) {}

  void capture_send(tpc::ParticipantId destination, SimValue value) {
    const auto current = io_count_++;
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
    const auto current = io_count_++;
    if (current < target_io_) {
      const auto idx = trace_offset_ + trace_consume_count_++;
      return trace_value(idx);
    }

    result_ = NondeterministicChoiceLabel{
        .value = {},
        .choices = std::vector<SimValue>(choices),
    };
    throw StepBoundaryReached{};
  }

  [[nodiscard]] ReceiveResult receive_step(tpc::Environment& env) {
    const auto current = io_count_++;
    if (current < target_io_) {
      const auto idx = trace_offset_ + trace_consume_count_++;
      const auto& observed = trace_entry(idx);
      if (observed.is_bottom()) {
        return ReceiveResult::replayed_timer_fire(fire_active_timer(env));
      }
      return ReceiveResult::replayed_message(decode_sim_message(trace_value(idx)));
    }

    result_ = has_active_timer() ? dpor::model::make_nonblocking_receive_label<SimValue>()
                                 : dpor::model::make_receive_label<SimValue>();
    return ReceiveResult::captured_receive();
  }

  void set_timer(tpc::TimerId id, tpc::TimerCallback callback) {
    if (active_timer_.has_value() && active_timer_->id != id) {
      throw SimulationFailure("SimEnvironment supports at most one active timer per thread");
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
      throw SimulationFailure("trace shorter than expected for simulated replay");
    }
    return trace_[idx];
  }

  [[nodiscard]] const SimValue& trace_value(std::size_t idx) const {
    const auto& observed = trace_entry(idx);
    if (observed.is_bottom()) {
      throw SimulationFailure("trace requested a concrete value but observed bottom");
    }
    return observed.value();
  }

  [[nodiscard]] tpc::Message decode_sim_message(SimValue value) const {
    try {
      return decode_message(value);
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& ex) {
      throw SimulationFailure(ex.what());
    }
  }

  [[nodiscard]] bool fire_active_timer(tpc::Environment& env) {
    if (!active_timer_.has_value()) {
      throw SimulationFailure("trace requested timer firing but no timer is active");
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

}  // namespace detail

class Environment : public tpc::Environment {
 public:
  Environment(std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map,
              std::size_t target_io, const ThreadTrace& trace, std::size_t trace_offset,
              bool inject_crash)
      : replay_(std::move(id_map), target_io, trace, trace_offset), inject_crash_(inject_crash) {}

  void send(tpc::ParticipantId destination, const tpc::Message& msg) override {
    if (inject_crash_ && !crash_injected_ && std::holds_alternative<tpc::DecisionMsg>(msg)) {
      crash_injected_ = true;
      const auto choice =
          replay_.replay_choice_or_capture({crash_choice(false), crash_choice(true)});
      if (decode_crash_choice(choice)) {
        throw detail::ScenarioThreadTerminated{};
      }
    }

    replay_.capture_send(destination, encode_message(msg));
  }

  tpc::Vote get_vote() override {
    return decode_vote_choice(replay_.replay_choice_or_capture(
        {vote_choice(tpc::Vote::Yes), vote_choice(tpc::Vote::No)}));
  }

  void set_timer(tpc::TimerId id, std::size_t /*timeout_ms*/,
                 tpc::TimerCallback callback) override {
    replay_.set_timer(id, std::move(callback));
  }

  void cancel_timer(tpc::TimerId id) override { replay_.cancel_timer(id); }

  [[nodiscard]] detail::ReceiveResult sim_receive() { return replay_.receive_step(*this); }

  [[nodiscard]] std::optional<EventLabel> result() const { return replay_.result(); }

 private:
  detail::ReplayState replay_;
  bool inject_crash_;
  bool crash_injected_{false};
};

template <typename ProtocolObj>
std::optional<EventLabel> run_and_capture(ProtocolObj& obj, Environment& env) {
  try {
    bool needs_message = false;
    if (const auto error =
            detail::invoke_protocol_step(needs_message, [&]() { return obj.start(env); });
        error.has_value()) {
      return error;
    }

    while (needs_message) {
      auto receive_step = env.sim_receive();
      if (receive_step.kind == detail::ReceiveResult::Kind::CapturedReceive) {
        return env.result();
      }
      if (receive_step.kind == detail::ReceiveResult::Kind::ReplayedTimerFire) {
        needs_message = receive_step.needs_message;
        continue;
      }
      if (const auto error = detail::invoke_protocol_step(
              needs_message, [&]() { return obj.receive(env, *receive_step.message); });
          error.has_value()) {
        return error;
      }
    }
    return std::nullopt;
  } catch (const detail::ScenarioThreadTerminated&) {
    return std::nullopt;
  } catch (const detail::StepBoundaryReached&) {
    return env.result();
  }
}

[[nodiscard]] inline ThreadFunction make_coordinator_function(Options options) {
  return [options](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    Environment env(participant_to_thread, step, trace, 0, options.inject_crash);
    std::optional<tpc::Coordinator> coordinator;
    if (const auto error = detail::try_emplace_protocol_object(
            coordinator, options.num_participants, options.bug_on_p1_no);
        error.has_value()) {
      return error;
    }
    return run_and_capture(*coordinator, env);
  };
}

[[nodiscard]] inline ThreadFunction make_participant_function(tpc::ParticipantId pid) {
  return [pid](const ThreadTrace& trace, std::size_t step) -> std::optional<EventLabel> {
    Environment env(participant_to_thread, step, trace, 0, /*inject_crash=*/false);
    std::optional<tpc::Participant> participant;
    if (const auto error = detail::try_emplace_protocol_object(participant, pid);
        error.has_value()) {
      return error;
    }
    return run_and_capture(*participant, env);
  };
}

[[nodiscard]] inline Program make_program(Options options) {
  Program program;
  program.threads[participant_to_thread(tpc::kCoordinator)] = make_coordinator_function(options);
  for (std::size_t pid = 1; pid <= options.num_participants; ++pid) {
    program.threads[participant_to_thread(pid)] = make_participant_function(pid);
  }
  return program;
}

}  // namespace tpc_sim::crash_before_decision
