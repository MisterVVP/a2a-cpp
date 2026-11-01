// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "curl_windows_readiness.h"

#if defined(_WIN32) && defined(A2A_HAS_LIBCURL)

#include <curl/curl.h>

#include <array>
#include <bit>
#include <limits>
#include <new>

namespace a2a::http::detail {
namespace {

constexpr WORD kWinsockVersion = MAKEWORD(2, 2);
constexpr int kWakeBacklog = 1;
constexpr int kEnabledSocketOption = 1;
constexpr char kWakeByte = 1;
constexpr std::size_t kWakeDrainBufferSize = 4096U;

void CloseSocket(SOCKET socket) noexcept {
  if (socket != INVALID_SOCKET) {
    (void)closesocket(socket);
  }
}

bool DisableInheritance(SOCKET socket) noexcept {
  // These are opaque Windows handle representations, not storage addresses.
  return SetHandleInformation(std::bit_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0) != 0;
}

SOCKET CreateWakeSocket() {
  const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket != INVALID_SOCKET && !DisableInheritance(socket)) {
    CloseSocket(socket);
    return INVALID_SOCKET;
  }
  return socket;
}

bool SetNonblocking(SOCKET socket) noexcept {
  u_long enabled = 1;
  return ioctlsocket(socket, static_cast<long>(FIONBIO), &enabled) == 0;
}

bool ConnectWakeSockets(SOCKET listener, SOCKET writer) {
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(listener, kWakeBacklog) != 0) {
    return false;
  }
  int address_size = sizeof(address);
  return getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_size) == 0 &&
         connect(writer, reinterpret_cast<const sockaddr*>(&address), address_size) == 0;
}

}  // namespace

CurlWindowsReadiness::CurlWindowsReadiness() {
  // Allocate before acquiring Winsock/socket resources: a throwing constructor
  // does not run this object's destructor.
  sockets_.push_back(WSAPOLLFD{.fd = INVALID_SOCKET, .events = POLLRDNORM, .revents = 0});
  WSADATA data{};
  winsock_started_ = WSAStartup(kWinsockVersion, &data) == 0;
  if (winsock_started_ && CreateWakeSockets()) {
    sockets_.front().fd = wake_reader_;
  } else {
    sockets_.clear();
  }
}

CurlWindowsReadiness::~CurlWindowsReadiness() {
  CloseSocket(wake_writer_);
  CloseSocket(wake_reader_);
  if (winsock_started_) {
    (void)WSACleanup();
  }
}

bool CurlWindowsReadiness::CreateWakeSockets() {
  // Private wake handles must not escape into child processes.
  const SOCKET listener = CreateWakeSocket();
  wake_writer_ = CreateWakeSocket();
  const bool connected =
      listener != INVALID_SOCKET && wake_writer_ != INVALID_SOCKET && ConnectWakeSockets(listener, wake_writer_);
  if (connected) {
    wake_reader_ = accept(listener, nullptr, nullptr);
  }
  CloseSocket(listener);
  return wake_reader_ != INVALID_SOCKET && DisableInheritance(wake_reader_) && SetNonblocking(wake_reader_) &&
         SetNonblocking(wake_writer_) &&
         setsockopt(wake_writer_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&kEnabledSocketOption),
                    sizeof(kEnabledSocketOption)) == 0;
}

bool CurlWindowsReadiness::valid() const noexcept { return !sockets_.empty(); }

bool CurlWindowsReadiness::Update(SOCKET socket, int curl_action) {
  if (curl_action == CURL_POLL_REMOVE || curl_action == CURL_POLL_NONE) {
    Remove(socket);
    return true;
  }
  short events = 0;
  events |= curl_action == CURL_POLL_IN || curl_action == CURL_POLL_INOUT ? POLLRDNORM : 0;
  events |= curl_action == CURL_POLL_OUT || curl_action == CURL_POLL_INOUT ? POLLWRNORM : 0;
  if (const auto found = registrations_.find(socket); found != registrations_.end()) {
    sockets_[found->second.index].events = events;
    found->second.generation = ++next_generation_;
    return true;
  }
  // No FD_SETSIZE/MAXIMUM_WAIT_OBJECTS restriction. Only the WSAPoll ABI's
  // ULONG count bounds this vector, independently of the number of streams.
  if (sockets_.size() >= (std::numeric_limits<ULONG>::max)()) {
    return false;
  }
  registrations_.emplace(socket, Registration{.index = sockets_.size(), .generation = ++next_generation_});
  try {
    sockets_.push_back(WSAPOLLFD{.fd = socket, .events = events, .revents = 0});
  } catch (...) {
    registrations_.erase(socket);
    throw;
  }
  return true;
}

void CurlWindowsReadiness::Remove(SOCKET socket) {
  const auto found = registrations_.find(socket);
  if (found == registrations_.end()) {
    return;
  }
  const auto index = found->second.index;
  if (index != sockets_.size() - 1U) {
    sockets_[index] = sockets_.back();
    registrations_.at(sockets_[index].fd).index = index;
  }
  sockets_.pop_back();
  registrations_.erase(found);
}

void CurlWindowsReadiness::Wake() const noexcept {
  if (wake_pending_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  int result = 0;
  do {
    result = send(wake_writer_, &kWakeByte, sizeof(kWakeByte), 0);
  } while (result == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
  if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
    // A full buffer already represents a pending wake. A fatal send error
    // instead makes the reader observe closure and terminates the reactor.
    (void)shutdown(wake_writer_, SD_BOTH);
  }
}

bool CurlWindowsReadiness::Wait() {
  int result = 0;
  do {
    result = WSAPoll(sockets_.data(), static_cast<ULONG>(sockets_.size()), deadline_.WaitMilliseconds());
  } while (result == SOCKET_ERROR && WSAGetLastError() == WSAEINTR);
  if (result == SOCKET_ERROR) {
    return false;
  }
  try {
    CollectEvents();
  } catch (const std::bad_alloc&) {
    return false;
  }
  return true;
}

void CurlWindowsReadiness::CollectEvents() {
  events_.clear();
  for (const auto& socket : sockets_) {
    if (socket.revents != 0 && socket.fd != wake_reader_) {
      events_.push_back(
          Event{.socket = socket.fd, .events = socket.revents, .generation = registrations_.at(socket.fd).generation});
    }
  }
}

bool CurlWindowsReadiness::DrainWake() const noexcept {
  std::array<char, kWakeDrainBufferSize> buffer{};
  int result = 0;
  do {
    result = recv(wake_reader_, buffer.data(), static_cast<int>(buffer.size()), 0);
  } while (result > 0 || (result == SOCKET_ERROR && WSAGetLastError() == WSAEINTR));
  // Clear before the reactor swaps its command queue. Producers coalesced
  // during draining are included in that swap; later producers send a byte.
  wake_pending_.store(false, std::memory_order_release);
  return result == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK;
}

bool CurlWindowsReadiness::IsCurrent(const Event& event) const noexcept {
  const auto found = registrations_.find(event.socket);
  return found != registrations_.end() && found->second.generation == event.generation;
}

const std::vector<CurlWindowsReadiness::Event>& CurlWindowsReadiness::events() const noexcept { return events_; }

CurlReactorDeadline& CurlWindowsReadiness::deadline() noexcept { return deadline_; }

}  // namespace a2a::http::detail

#endif
