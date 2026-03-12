#pragma once

#include "bridge.hpp"
#include "core.hpp"

#include <functional>
#include <optional>
#include <utility>

namespace tpc_sim {

class NondeterministicVoteEnvironment : public tpc::Environment {
 public:
  NondeterministicVoteEnvironment(
      std::function<dpor::model::ThreadId(tpc::ParticipantId)> id_map,
      std::size_t target_io,
      const ThreadTrace& trace,
      std::size_t trace_offset)
      : core_(std::move(id_map), target_io, trace, trace_offset) {}

  void send(tpc::ParticipantId destination, const tpc::Message& msg) override {
    core_.capture_send(destination, encode_message(msg));
  }

  tpc::Vote get_vote() override {
    return decode_vote_choice(core_.replay_choice_or_capture(
        {vote_choice(tpc::Vote::Yes), vote_choice(tpc::Vote::No)}));
  }

  void set_timer(tpc::TimerId id, std::size_t /*timeout_ms*/,
                 tpc::TimerCallback callback) override {
    core_.set_timer(id, std::move(callback));
  }

  void cancel_timer(tpc::TimerId id) override {
    core_.cancel_timer(id);
  }

  [[nodiscard]] SimReceiveResult sim_receive() {
    return core_.receive_step(*this);
  }

  [[nodiscard]] std::optional<EventLabel> result() const {
    return core_.result();
  }

 private:
  ReplayCore core_;
};

}  // namespace tpc_sim
