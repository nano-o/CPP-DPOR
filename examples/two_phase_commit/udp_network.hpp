#pragma once

// Real UDP-based Environment implementation for Two-Phase Commit.
//
// Simple datagram transport with no retransmission.  Each node binds
// to its own (host, port) pair and sends/receives serialized messages.
// A background thread receives datagrams and enqueues them; the
// environment drives the protocol by dequeuing one message at a time.

#include "protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace tpc {

class UdpEnvironment : public Environment {
 public:
  UdpEnvironment(
      ParticipantId my_id,
      std::unordered_map<ParticipantId, std::pair<std::string, uint16_t>>
          port_map,
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

    if (::bind(socket_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
        0) {
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
    if (::inet_pton(AF_INET, it->second.first.c_str(), &dest_addr.sin_addr) !=
        1) {
      throw std::invalid_argument("invalid destination address: " +
                                  it->second.first);
    }

    std::string data = serialize(msg);
    if (::sendto(socket_fd_, data.data(), data.size(), 0,
                 reinterpret_cast<sockaddr*>(&dest_addr),
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

  // Drive a protocol to completion. Starts a background receiver thread,
  // calls protocol.start(), then feeds messages from the queue until done.
  // Single-use: calling run() more than once throws std::logic_error.
  template <typename Protocol>
  void run(Protocol& protocol) {
    if (ran_) {
      throw std::logic_error("UdpEnvironment::run() called more than once");
    }
    ran_ = true;

    std::thread receiver_thread([this] { receiver_loop(); });

    try {
      bool needs_message = protocol.start(*this);
      while (needs_message) {
        Message msg = dequeue();
        needs_message = protocol.receive(*this, msg);
      }
    } catch (...) {
      stop_receiver();
      receiver_thread.join();
      throw;
    }

    stop_receiver();
    receiver_thread.join();
  }

 private:
  void receiver_loop() {
    while (true) {
      std::array<char, 1024> buf{};
      auto n =
          ::recvfrom(socket_fd_, buf.data(), buf.size(), 0, nullptr, nullptr);
      if (n <= 0) {
        break;
      }
      try {
        auto msg = deserialize(
            std::string(buf.data(), static_cast<std::size_t>(n)));
        {
          std::lock_guard<std::mutex> lock(queue_mutex_);
          queue_.push(std::move(msg));
        }
        queue_cv_.notify_one();
      } catch (const std::invalid_argument&) {
        // Skip malformed datagrams.
      }
    }
    // Signal dequeue() so it doesn't block forever.
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      receiver_stopped_ = true;
    }
    queue_cv_.notify_one();
  }

  Message dequeue() {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait(lock,
                   [this] { return !queue_.empty() || receiver_stopped_; });
    if (queue_.empty()) {
      throw std::runtime_error("receiver thread stopped unexpectedly");
    }
    Message msg = std::move(queue_.front());
    queue_.pop();
    return msg;
  }

  void stop_receiver() {
    // Shut down the socket to unblock the recvfrom() in the receiver thread.
    ::shutdown(socket_fd_, SHUT_RDWR);
  }

  ParticipantId my_id_;
  int socket_fd_{-1};
  std::unordered_map<ParticipantId, std::pair<std::string, uint16_t>>
      port_map_;
  std::optional<Vote> vote_;

  // Async receive queue.
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::queue<Message> queue_;
  bool receiver_stopped_ = false;
  bool ran_ = false;
};

}  // namespace tpc
