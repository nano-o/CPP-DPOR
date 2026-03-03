#pragma once

// Real UDP-based Environment implementation for Two-Phase Commit.
//
// Simple datagram transport with no retransmission.  Each node binds
// to its own (host, port) pair and sends/receives serialized messages.

#include "protocol.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace tpc {

class UdpEnvironment : public Environment {
 public:
  UdpEnvironment(
      ParticipantId my_id,
      std::unordered_map<ParticipantId, std::pair<std::string, uint16_t>>
          port_map,
      Vote vote = Vote::Yes)
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

  Message receive() override {
    std::array<char, 1024> buf{};
    auto n =
        ::recvfrom(socket_fd_, buf.data(), buf.size(), 0, nullptr, nullptr);
    if (n <= 0) {
      throw std::runtime_error("recvfrom() failed");
    }
    return deserialize(
        std::string(buf.data(), static_cast<std::size_t>(n)));
  }

  Vote get_vote() override { return vote_; }

 private:
  ParticipantId my_id_;
  int socket_fd_{-1};
  std::unordered_map<ParticipantId, std::pair<std::string, uint16_t>>
      port_map_;
  Vote vote_;
};

}  // namespace tpc
