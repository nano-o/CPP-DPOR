#pragma once

// Real UDP-based Environment implementation for Two-Phase Commit.
//
// Simple datagram transport with no retransmission. Each node binds
// to its own (host, port) pair and sends/receives serialized messages.
// run() drives both socket I/O and timers on the same thread via poll().

#include "protocol.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace tpc {

class UdpEnvironment : public Environment {
 public:
  UdpEnvironment(ParticipantId my_id,
                 std::unordered_map<ParticipantId, std::pair<std::string, uint16_t>> port_map,
                 std::optional<Vote> vote = std::nullopt)
      : my_id_(my_id), port_map_(std::move(port_map)), vote_(vote) {
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
      throw std::runtime_error("socket() failed");
    }

    auto it = port_map_.find(my_id_);
    if (it == port_map_.end()) {
      ::close(socket_fd_);
      throw std::invalid_argument("my_id not found in port_map");
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(it->second.second);
    if (::inet_pton(AF_INET, it->second.first.c_str(), &addr.sin_addr) != 1) {
      ::close(socket_fd_);
      throw std::invalid_argument("invalid address: " + it->second.first);
    }

    if (::bind(socket_fd_,
               reinterpret_cast<sockaddr*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                   &addr),
               sizeof(addr)) < 0) {
      ::close(socket_fd_);
      throw std::runtime_error("bind() failed");
    }
  }

  ~UdpEnvironment() override { ::close(socket_fd_); }

  UdpEnvironment(const UdpEnvironment&) = delete;
  UdpEnvironment& operator=(const UdpEnvironment&) = delete;

  void send(ParticipantId destination, const Message& msg) override {
    auto it = port_map_.find(destination);
    if (it == port_map_.end()) {
      throw std::invalid_argument("unknown destination");
    }

    sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(it->second.second);
    if (::inet_pton(AF_INET, it->second.first.c_str(), &dest_addr.sin_addr) != 1) {
      throw std::invalid_argument("invalid destination address: " + it->second.first);
    }

    std::string data = serialize(msg);
    if (::sendto(
            socket_fd_, data.data(), data.size(), 0,
            reinterpret_cast<sockaddr*>(  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                &dest_addr),
            sizeof(dest_addr)) < 0) {
      throw std::runtime_error("sendto() failed");
    }
  }

  Vote get_vote() override {
    if (vote_) {
      return *vote_;
    }
    thread_local std::mt19937 rng{std::random_device{}()};
    std::bernoulli_distribution dist(0.5);
    return dist(rng) ? Vote::Yes : Vote::No;
  }

  void set_timer(TimerId id, std::size_t timeout_ms, TimerCallback callback) override {
    require_run_thread("set_timer");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto it = active_timers_.find(id);
    if (it != active_timers_.end()) {
      timer_schedule_.erase(it->second.schedule_it);
      active_timers_.erase(it);
    }
    auto schedule_it = timer_schedule_.emplace(deadline, id);
    active_timers_.emplace(id, ActiveTimer{schedule_it, std::move(callback)});
  }

  void cancel_timer(TimerId id) override {
    require_run_thread("cancel_timer");
    auto it = active_timers_.find(id);
    if (it != active_timers_.end()) {
      timer_schedule_.erase(it->second.schedule_it);
      active_timers_.erase(it);
    }
  }

  // Drive a protocol to completion. Runs socket I/O and timer dispatch
  // on the same thread.
  // Single-use: calling run() more than once throws std::logic_error.
  template <typename Protocol>
  void run(Protocol& protocol) {
    if (ran_) {
      throw std::logic_error("UdpEnvironment::run() called more than once");
    }
    ran_ = true;
    run_active_ = true;
    run_thread_id_ = std::this_thread::get_id();

    try {
      bool needs_message = protocol.start(*this);
      while (needs_message) {
        needs_message = dispatch_due_timers();
        if (!needs_message) {
          break;
        }
        auto msg = wait_for_message(compute_next_timeout_ms());
        if (msg.has_value()) {
          needs_message = protocol.receive(*this, *msg);
        }
      }
    } catch (...) {
      reset_timer_state();
      run_active_ = false;
      throw;
    }

    reset_timer_state();
    run_active_ = false;
  }

 private:
  using TimerDeadline = std::chrono::steady_clock::time_point;
  using TimerSchedule = std::multimap<TimerDeadline, TimerId>;

  struct ActiveTimer {
    TimerSchedule::iterator schedule_it;
    TimerCallback callback;
  };

  void require_run_thread(const char* op) const {
    if (!run_active_ || std::this_thread::get_id() != run_thread_id_) {
      throw std::logic_error(std::string("UdpEnvironment::") + op +
                             " must be called from run() thread");
    }
  }

  int compute_next_timeout_ms() const {
    if (timer_schedule_.empty()) {
      return -1;
    }
    auto now = std::chrono::steady_clock::now();
    auto delta = timer_schedule_.begin()->first - now;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(delta);
    auto count = std::max(ms.count(), static_cast<std::chrono::milliseconds::rep>(0));
    if (count > static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max())) {
      return std::numeric_limits<int>::max();
    }
    return static_cast<int>(count);
  }

  bool dispatch_due_timers() {
    while (!timer_schedule_.empty()) {
      auto now = std::chrono::steady_clock::now();
      auto schedule_it = timer_schedule_.begin();
      if (schedule_it->first > now) {
        break;
      }

      auto active_it = active_timers_.find(schedule_it->second);
      if (active_it == active_timers_.end() || active_it->second.schedule_it != schedule_it) {
        timer_schedule_.erase(schedule_it);
        continue;
      }

      TimerCallback callback = std::move(active_it->second.callback);
      timer_schedule_.erase(schedule_it);
      active_timers_.erase(active_it);
      // Callback may set/cancel timers; this is safe because the current timer
      // has already been removed from both containers before invocation.
      if (!callback(*this)) {
        return false;
      }
    }
    return true;
  }

  std::optional<Message> wait_for_message(int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    int remaining_ms = timeout_ms;
    auto refresh_remaining_timeout = [&]() -> bool {
      if (timeout_ms < 0) {
        return true;
      }
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      if (elapsed >= static_cast<std::chrono::milliseconds::rep>(timeout_ms)) {
        remaining_ms = 0;
        return false;
      }
      auto remaining = static_cast<std::chrono::milliseconds::rep>(timeout_ms) - elapsed;
      if (remaining >
          static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max())) {
        remaining_ms = std::numeric_limits<int>::max();
      } else {
        remaining_ms = static_cast<int>(remaining);
      }
      return true;
    };

    while (true) {
      pollfd fd{};
      fd.fd = socket_fd_;
      fd.events = POLLIN;

      int ret = ::poll(&fd, 1, remaining_ms);
      if (ret < 0) {
        if (errno == EINTR) {
          if (!refresh_remaining_timeout()) {
            return std::nullopt;
          }
          continue;
        }
        throw std::runtime_error("poll() failed");
      }
      if (ret == 0) {
        return std::nullopt;
      }

      if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        throw std::runtime_error("socket poll error");
      }
      if (!(fd.revents & POLLIN)) {
        if (!refresh_remaining_timeout()) {
          return std::nullopt;
        }
        continue;
      }

      std::array<char, 1024> buf{};
      auto n = ::recvfrom(socket_fd_, buf.data(), buf.size(), 0, nullptr, nullptr);
      if (n < 0) {
        if (errno == EINTR) {
          if (!refresh_remaining_timeout()) {
            return std::nullopt;
          }
          continue;
        }
        throw std::runtime_error("recvfrom() failed");
      }
      if (n == 0) {
        if (!refresh_remaining_timeout()) {
          return std::nullopt;
        }
        continue;
      }
      try {
        return deserialize(std::string(buf.data(), static_cast<std::size_t>(n)));
      } catch (const std::invalid_argument&) {
        // Skip malformed datagrams and keep waiting.
        if (!refresh_remaining_timeout()) {
          return std::nullopt;
        }
      }
    }
  }

  void reset_timer_state() {
    active_timers_.clear();
    timer_schedule_.clear();
  }

  ParticipantId my_id_;
  int socket_fd_{-1};
  std::unordered_map<ParticipantId, std::pair<std::string, uint16_t>> port_map_;
  std::optional<Vote> vote_;

  TimerSchedule timer_schedule_;
  std::unordered_map<TimerId, ActiveTimer> active_timers_;

  bool ran_ = false;
  bool run_active_ = false;
  std::thread::id run_thread_id_{};
};

}  // namespace tpc
