// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/tck_sut_http_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "a2a/server/http_adapter.h"
#include "a2a/server/network_utils.h"
#include "a2a/server/transport_mux.h"
#include "sut/sut_runtime.h"

namespace a2a::tests::sut {
namespace {

constexpr int kListenBacklog = 128;
constexpr int kReuseAddress = 1;
constexpr int kAcceptRetryDelayMillis = 1;
#ifdef _WIN32
constexpr long kReadPollTimeoutMicroseconds = 100'000;
#endif
constexpr std::string_view kSocketReceiveFailureMessage = "Socket recv failed";
const std::string kHttpHostHeader = "localhost";

class SocketTransport final : public server::HttpByteTransport {
 public:
  SocketTransport(int fd, const std::atomic_bool& shutdown_requested)
      : fd_(fd), shutdown_requested_(shutdown_requested) {
    (void)server::SetSocketNoDelay(fd_);
  }

  core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
#ifdef _WIN32
    // Winsock shutdown does not reliably interrupt an already-blocked recv.
    // Poll before reading so idle connections observe cancellation without closing
    // a handle concurrently with I/O or losing the worker's socket ownership.
    while (!shutdown_requested_.load()) {
      fd_set readable{};
      FD_ZERO(&readable);
      FD_SET(static_cast<SOCKET>(fd_), &readable);
      timeval timeout{0, kReadPollTimeoutMicroseconds};
      const int ready = ::select(0, &readable, nullptr, nullptr, &timeout);
      if (ready == SOCKET_ERROR) {
        return core::Error::Internal(std::string(kSocketReceiveFailureMessage));
      }
      if (ready > 0) {
        break;
      }
    }
#endif
    if (shutdown_requested_.load()) {
      return std::size_t{0};
    }
    const auto bytes = ::recv(fd_, buffer, size, 0);
    if (bytes < 0) {
      return core::Error::Internal(std::string(kSocketReceiveFailureMessage));
    }
    return static_cast<std::size_t>(bytes);
  }

  core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    const auto bytes = ::send(fd_, buffer, size, 0);
    if (bytes < 0) {
      return core::Error::Internal("Socket send failed");
    }
    return static_cast<std::size_t>(bytes);
  }

 private:
  int fd_;
  const std::atomic_bool& shutdown_requested_;
};

class HttpConnectionRegistry final {
 public:
  void Add(int fd) {
    std::lock_guard lock(mutex_);
    active_fds_.insert(fd);
  }
  void Remove(int fd) {
    std::lock_guard lock(mutex_);
    active_fds_.erase(fd);
  }
  void ShutdownActiveSockets() {
    shutdown_requested_.store(true);
    std::lock_guard lock(mutex_);
    for (const int fd : active_fds_) {
#ifdef _WIN32
      (void)::shutdown(fd, SD_BOTH);
#else
      (void)::shutdown(fd, SHUT_RDWR);
#endif
    }
  }
  [[nodiscard]] const std::atomic_bool& ShutdownRequested() const noexcept { return shutdown_requested_; }

 private:
  std::atomic_bool shutdown_requested_{false};
  std::mutex mutex_;
  std::unordered_set<int> active_fds_;
};

[[nodiscard]] core::Result<server::HttpServerResponse> RouteRequest(const server::HttpServerRequest& request,
                                                                    const server::TransportMux& mux,
                                                                    SutHttpConnectionObserver* observer,
                                                                    bool& handled_by_observer) {
  if (observer == nullptr) {
    return mux.RouteRequest(request);
  }
  server::HttpServerResponse observer_response;
  handled_by_observer = observer->BeginRequest(request, observer_response);
  if (handled_by_observer) {
    return observer_response;
  }
  return mux.RouteRequest(request);
}

[[nodiscard]] bool SetAcceptedSocketBlocking(int fd) {
#ifdef _WIN32
  u_long mode = 0UL;
  return ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode) == 0;
#else
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
#endif
}

void HandleHttpConnection(int fd, const server::TransportMux& mux, HttpConnectionRegistry& registry,
                          std::unique_ptr<SutHttpConnectionObserver> observer) {
  // BSD sockets and Winsock inherit the listener's non-blocking mode.
  // The adapter uses blocking I/O, including while a persistent connection is idle.
  if (!SetAcceptedSocketBlocking(fd)) {
    registry.Remove(fd);
    server::CloseSocketCrossPlatform(fd);
    return;
  }
  SocketTransport socket_transport(fd, registry.ShutdownRequested());
  const server::HttpAdapter adapter;
  server::HttpConnectionState connection_state;
  while (true) {
    auto parsed = adapter.ReadRequest(socket_transport, connection_state, kHttpHostHeader);
    if (!parsed.ok()) {
      break;
    }
    server::HttpServerRequest request = std::move(parsed.value());
    bool handled_by_observer = false;
    auto response = RouteRequest(request, mux, observer.get(), handled_by_observer);
    if (!response.ok()) {
      break;
    }
    const bool close_connection = server::HttpAdapter::ShouldCloseConnection(request, response.value());
    const auto written = server::HttpAdapter::WriteResponse(socket_transport, response.value(), close_connection);
    if (!written.ok()) {
      break;
    }
    if (observer != nullptr) {
      observer->FinishRequest(response.value(), close_connection, handled_by_observer);
    }
    if (close_connection) {
      break;
    }
  }
  registry.Remove(fd);
  server::CloseSocketCrossPlatform(fd);
}

}  // namespace

class TckHttpServer::Impl final {
 public:
  Impl(std::string_view host, int port, const server::TransportMux& mux, SutRuntimeObserver* observer)
      : host_(host), port_(port), mux_(mux), observer_(observer) {}

  ~Impl() {
    CloseListener();
    registry_.ShutdownActiveSockets();
    JoinConnections();
#ifdef _WIN32
    if (winsock_started_) {
      WSACleanup();
    }
#endif
  }

  [[nodiscard]] bool Start();
  void AcceptConnections(const volatile std::sig_atomic_t& keep_running);
  void CloseListener();
  void ShutdownActiveSockets() { registry_.ShutdownActiveSockets(); }
  void JoinConnections();

  std::string host_;
  int port_;
  const server::TransportMux& mux_;
  int server_fd_ = -1;
  HttpConnectionRegistry registry_;
  SutRuntimeObserver* observer_;
  std::vector<std::thread> connection_threads_;
#ifdef _WIN32
  bool winsock_started_ = false;
#endif
};

bool TckHttpServer::Impl::Start() {
#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    return false;
  }
  winsock_started_ = true;
#endif
  server_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    std::cerr << "Failed to create HTTP listening socket: " << std::strerror(errno) << '\n';
    return false;
  }
  int option = kReuseAddress;
  if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&option), sizeof(option)) != 0) {
    std::cerr << "Failed to configure HTTP listening socket reuse: " << std::strerror(errno) << '\n';
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port_));
  if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) {
    std::cerr << "Invalid IPv4 host for TCK SUT HTTP listener: " << host_ << '\n';
    return false;
  }
  if (bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "Failed to bind TCK SUT HTTP listener on " << host_ << ':' << port_ << ": " << std::strerror(errno)
              << '\n';
    return false;
  }
  if (listen(server_fd_, kListenBacklog) != 0) {
    std::cerr << "Failed to listen on TCK SUT HTTP endpoint " << host_ << ':' << port_ << ": " << std::strerror(errno)
              << '\n';
    return false;
  }
  if (!server::SetSocketNonBlocking(server_fd_)) {
    std::cerr << "Failed to configure non-blocking TCK SUT HTTP listener: " << std::strerror(errno) << '\n';
    return false;
  }
  return true;
}

void TckHttpServer::Impl::AcceptConnections(const volatile std::sig_atomic_t& keep_running) {
  while (keep_running != 0) {
    sockaddr_in client{};
#ifdef _WIN32
    int length = sizeof(client);
#else
    socklen_t length = sizeof(client);
#endif
    const int fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&client), &length);
    if (fd >= 0) {
      registry_.Add(fd);
      auto connection_observer = observer_ == nullptr ? nullptr : observer_->ObserveHttpConnection();
      connection_threads_.emplace_back(HandleHttpConnection, fd, std::cref(mux_), std::ref(registry_),
                                       std::move(connection_observer));
      continue;
    }
#ifdef _WIN32
    const int accept_error = WSAGetLastError();
    if (accept_error != WSAEWOULDBLOCK && accept_error != WSAEINTR) {
      std::cerr << "TCK SUT HTTP accept failed with Winsock error: " << accept_error << '\n';
      break;
    }
#else
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      std::cerr << "TCK SUT HTTP accept failed: " << std::strerror(errno) << '\n';
      break;
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptRetryDelayMillis));
  }
  CloseListener();
}

void TckHttpServer::Impl::CloseListener() {
  if (server_fd_ >= 0) {
    server::CloseSocketCrossPlatform(server_fd_);
    server_fd_ = -1;
  }
}

void TckHttpServer::Impl::JoinConnections() {
  for (auto& thread : connection_threads_) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

TckHttpServer::TckHttpServer(std::string_view host, int port, const server::TransportMux& mux,
                             SutRuntimeObserver* observer)
    : impl_(std::make_unique<Impl>(host, port, mux, observer)) {}
TckHttpServer::~TckHttpServer() = default;
bool TckHttpServer::Start() { return impl_->Start(); }
void TckHttpServer::AcceptConnections(const volatile std::sig_atomic_t& keep_running) {
  impl_->AcceptConnections(keep_running);
}
void TckHttpServer::ShutdownActiveSockets() { impl_->ShutdownActiveSockets(); }
void TckHttpServer::JoinConnections() { impl_->JoinConnections(); }

}  // namespace a2a::tests::sut
