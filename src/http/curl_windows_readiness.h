// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#if defined(_WIN32) && defined(A2A_HAS_LIBCURL)

#include <winsock2.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "curl_reactor_deadline.h"

namespace a2a::http::detail {

// Only Wake() is callable from producer threads. The reactor owns membership,
// polling, deadlines and dispatch. Its lifetime encloses all producer calls.
class CurlWindowsReadiness final {
 public:
  struct Event final {
    SOCKET socket = INVALID_SOCKET;
    short events = 0;
    std::uint64_t generation = 0;
  };

  CurlWindowsReadiness();
  ~CurlWindowsReadiness();
  CurlWindowsReadiness(const CurlWindowsReadiness&) = delete;
  CurlWindowsReadiness& operator=(const CurlWindowsReadiness&) = delete;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] static int CurlAction(short events) noexcept;
  [[nodiscard]] bool Update(SOCKET socket, int curl_action);
  void Wake() const noexcept;
  [[nodiscard]] bool Wait();
  [[nodiscard]] bool DrainWake() const noexcept;
  [[nodiscard]] bool IsCurrent(const Event& event) const noexcept;
  [[nodiscard]] const std::vector<Event>& events() const noexcept;
  [[nodiscard]] CurlReactorDeadline& deadline() noexcept;

 private:
  struct Registration final {
    std::size_t index = 0;
    std::uint64_t generation = 0;
  };

  [[nodiscard]] bool CreateWakeSockets();
  void Remove(SOCKET socket);
  void CollectEvents();

  bool winsock_started_ = false;
  SOCKET wake_reader_ = INVALID_SOCKET;
  SOCKET wake_writer_ = INVALID_SOCKET;
  mutable std::atomic<bool> wake_pending_{false};
  // Index zero is permanently reserved for the wake reader. Swap removal
  // keeps updates O(1); WSAPoll and collection are O(number of sockets).
  std::vector<WSAPOLLFD> sockets_;
  std::unordered_map<SOCKET, Registration> registrations_;
  std::vector<Event> events_;
  std::uint64_t next_generation_ = 0;
  CurlReactorDeadline deadline_;
};

}  // namespace a2a::http::detail

#endif
