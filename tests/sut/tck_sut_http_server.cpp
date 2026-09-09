// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/tck_sut_http_server.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "a2a/core/http_constants.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif
#include "a2a/server/http_adapter.h"
#include "a2a/server/network_utils.h"
#include "a2a/server/transport_mux.h"
#include "sut/tck_sut.h"

namespace a2a::tests::sut {
namespace {

constexpr int kListenBacklog = 128;
constexpr int kReuseAddress = 1;
constexpr int kAcceptRetryDelayMillis = 1;
constexpr std::string_view kHttpDiagnosticsPrefix = "A2A_HTTP_DIAGNOSTICS";
const std::string kHttpHostHeader = "localhost";

class SocketTransport final : public server::HttpByteTransport {
 public:
  explicit SocketTransport(int fd) : fd_(fd) { (void)server::SetSocketNoDelay(fd_); }

  core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    const auto bytes = ::recv(fd_, buffer, size, 0);
    if (bytes < 0) {
      return core::Error::Internal("Socket recv failed");
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
    std::lock_guard lock(mutex_);
    for (const int fd : active_fds_) {
#ifdef _WIN32
      (void)::shutdown(fd, SD_BOTH);
#else
      (void)::shutdown(fd, SHUT_RDWR);
#endif
    }
  }

 private:
  std::mutex mutex_;
  std::unordered_set<int> active_fds_;
};

struct HttpDiagnostics final {
  std::atomic<std::uint64_t> accepted_unary_connections{0};
  std::atomic<std::uint64_t> completed_unary_operations{0};
  std::atomic<std::uint64_t> finite_stream_connections{0};
  std::atomic<std::uint64_t> completed_finite_streams{0};
  std::atomic<std::uint64_t> connections_reused_after_finite_stream{0};
};

[[nodiscard]] bool IsDiagnosticsResetRequest(const server::HttpServerRequest& request) {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  return request.method == core::http::kMethodPost && request.target == kDiagnosticsResetPath;
#else
  (void)request;
  return false;
#endif
}

[[nodiscard]] core::Result<server::HttpServerResponse> RouteRequest(const server::HttpServerRequest& request,
                                                                    const server::TransportMux& mux,
                                                                    bool is_diagnostics_reset) {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  if (is_diagnostics_reset) {
    (void)core::subscription_diagnostics::TakeSnapshot();
    server::HttpServerResponse response;
    response.status_code = core::http::kStatusNoContent;
    return response;
  }
#else
  (void)is_diagnostics_reset;
#endif
  return mux.RouteRequest(request);
}

void RecordResponse(const server::HttpServerResponse& response, bool close_connection,
                    bool& completed_unary_on_connection, bool& completed_finite_stream_on_connection,
                    bool& awaiting_request_after_finite_stream, HttpDiagnostics& diagnostics) {
  if (!response.stream_writer) {
    if (!completed_unary_on_connection) {
      completed_unary_on_connection = true;
      diagnostics.accepted_unary_connections.fetch_add(1, std::memory_order_relaxed);
    }
    diagnostics.completed_unary_operations.fetch_add(1, std::memory_order_relaxed);
  }
  if (response.stream_kind != server::HttpStreamKind::kFinite) {
    return;
  }
  if (!completed_finite_stream_on_connection) {
    completed_finite_stream_on_connection = true;
    diagnostics.finite_stream_connections.fetch_add(1, std::memory_order_relaxed);
  }
  diagnostics.completed_finite_streams.fetch_add(1, std::memory_order_relaxed);
  awaiting_request_after_finite_stream = !close_connection;
}

void HandleHttpConnection(int fd, const server::TransportMux& mux, HttpConnectionRegistry& registry,
                          HttpDiagnostics& diagnostics) {
  SocketTransport socket_transport(fd);
  const server::HttpAdapter adapter;
  server::HttpConnectionState connection_state;
  bool completed_unary_on_connection = false;
  bool completed_finite_stream_on_connection = false;
  bool awaiting_request_after_finite_stream = false;
  while (true) {
    auto parsed = adapter.ReadRequest(socket_transport, connection_state, kHttpHostHeader);
    if (!parsed.ok()) {
      break;
    }
    server::HttpServerRequest request = std::move(parsed.value());
    const bool is_diagnostics_reset = IsDiagnosticsResetRequest(request);
    if (!is_diagnostics_reset && awaiting_request_after_finite_stream) {
      diagnostics.connections_reused_after_finite_stream.fetch_add(1, std::memory_order_relaxed);
      awaiting_request_after_finite_stream = false;
    }
    auto response = RouteRequest(request, mux, is_diagnostics_reset);
    if (!response.ok()) {
      break;
    }
    const bool close_connection = server::HttpAdapter::ShouldCloseConnection(request, response.value());
    const auto written = server::HttpAdapter::WriteResponse(socket_transport, response.value(), close_connection);
    if (!written.ok()) {
      break;
    }
    if (!is_diagnostics_reset) {
      RecordResponse(response.value(), close_connection, completed_unary_on_connection,
                     completed_finite_stream_on_connection, awaiting_request_after_finite_stream, diagnostics);
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
  Impl(std::string_view host, int port, const server::TransportMux& mux) : host_(host), port_(port), mux_(mux) {}

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
  void EmitDiagnostics() const;

  std::string host_;
  int port_;
  const server::TransportMux& mux_;
  int server_fd_ = -1;
  HttpConnectionRegistry registry_;
  HttpDiagnostics diagnostics_;
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
      connection_threads_.emplace_back(HandleHttpConnection, fd, std::cref(mux_), std::ref(registry_),
                                       std::ref(diagnostics_));
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

void TckHttpServer::Impl::EmitDiagnostics() const {
  const std::uint64_t accepted = diagnostics_.accepted_unary_connections.load(std::memory_order_relaxed);
  const std::uint64_t unary = diagnostics_.completed_unary_operations.load(std::memory_order_relaxed);
  const std::uint64_t stream_connections = diagnostics_.finite_stream_connections.load(std::memory_order_relaxed);
  const std::uint64_t streams = diagnostics_.completed_finite_streams.load(std::memory_order_relaxed);
  const double operations_per_connection =
      accepted == 0U ? 0.0 : static_cast<double>(unary) / static_cast<double>(accepted);
  const double streams_per_connection =
      stream_connections == 0U ? 0.0 : static_cast<double>(streams) / static_cast<double>(stream_connections);
  std::cout << kHttpDiagnosticsPrefix << " accepted_connections=" << accepted << " completed_unary_operations=" << unary
            << " operations_per_connection=" << operations_per_connection
            << " finite_stream_connections=" << stream_connections << " completed_finite_streams=" << streams
            << " finite_streams_per_connection=" << streams_per_connection << " connections_reused_after_finite_stream="
            << diagnostics_.connections_reused_after_finite_stream.load(std::memory_order_relaxed) << '\n'
            << std::flush;
}

TckHttpServer::TckHttpServer(std::string_view host, int port, const server::TransportMux& mux)
    : impl_(std::make_unique<Impl>(host, port, mux)) {}
TckHttpServer::~TckHttpServer() = default;
bool TckHttpServer::Start() { return impl_->Start(); }
void TckHttpServer::AcceptConnections(const volatile std::sig_atomic_t& keep_running) {
  impl_->AcceptConnections(keep_running);
}
void TckHttpServer::ShutdownActiveSockets() { impl_->ShutdownActiveSockets(); }
void TckHttpServer::JoinConnections() { impl_->JoinConnections(); }
void TckHttpServer::EmitDiagnostics() const { impl_->EmitDiagnostics(); }

}  // namespace a2a::tests::sut
