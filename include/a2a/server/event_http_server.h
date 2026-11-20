// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#pragma once

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "a2a/core/non_copyable.h"
#include "a2a/server/engine/request_execution.h"
#include "a2a/server/http_adapter.h"

namespace lf::a2a::v1 {
class AgentCard;
}  // namespace lf::a2a::v1

namespace a2a::server {
namespace engine {
class AsyncDispatcher;
}  // namespace engine
struct ServerBindingsOptions;

// Await each write before producing another event. Consumption means the socket
// accepted the bytes, not that the peer application read them. Cancellation wakes
// pending writes. One bounded event is retained; no waiting worker is required.
class HttpEventStream {
 public:
  virtual ~HttpEventStream() = default;
  virtual boost::asio::awaitable<core::Result<void>> Write(std::string bytes) = 0;
};

struct EventHttpResponse final {
  // With a stream producer, body holds initial stream bytes sent before it runs.
  HttpServerResponse response;
  using Producer = std::function<boost::asio::awaitable<core::Result<void>>(std::shared_ptr<HttpEventStream>,
                                                                            engine::RequestExecutionContext)>;
  Producer stream;
};

struct EventHttpServerOptions final {
  static constexpr std::size_t kDefaultConnections = 1024;
  static constexpr std::size_t kDefaultPending = 128;
  static constexpr std::size_t kDefaultWorkers = 4;
  static constexpr std::size_t kDefaultBufferBytes = 1024U * 1024U;
  static constexpr auto kDefaultIdleTimeout = std::chrono::seconds(30);
  static constexpr auto kDefaultHeartbeat = std::chrono::seconds(15);
  static constexpr std::size_t kMaxNetworkWorkers = 2;
  static constexpr unsigned kDefaultNetworkAutoMinCpus = 4;
  static constexpr std::size_t kDefaultSecondNetworkConnectionThreshold = 4;
  std::string address = "127.0.0.1";
  std::uint16_t port = 0;
  std::size_t max_connections = kDefaultConnections;
  // Includes queued/running application calls and streams until cleanup returns.
  std::size_t max_pending_requests = kDefaultPending;
  std::size_t application_threads = kDefaultWorkers;
  std::size_t max_input_bytes = kDefaultBufferBytes;
  std::size_t max_output_bytes = kDefaultBufferBytes;
  HttpAdapter::Options codec = {};
  std::chrono::milliseconds idle_timeout = kDefaultIdleTimeout;
  std::chrono::milliseconds heartbeat_interval = kDefaultHeartbeat;
  // Select one or two socket workers. Zero selects automatically, with one worker for generic handlers.
  std::size_t network_workers = 0;
  // Minimum hardware concurrency for a second worker when a native binding uses automatic selection.
  unsigned network_auto_min_cpus = kDefaultNetworkAutoMinCpus;
  // Active connections before new connections alternate between socket workers. Zero enables immediate alternation.
  std::size_t second_network_connection_threshold = kDefaultSecondNetworkConnectionThreshold;
};

// Optional a2a::asio listener. Linux epoll, macOS kqueue, Windows IOCP through
// Boost.Asio. Each connection stays on one network thread. Application code
// runs on the application pool or the native dispatcher. Start is single-use.
// Shutdown is thread-safe and non-blocking. Join/destruction must run outside
// callbacks, drain cancellation and wait for legacy blocking work to return.
class EventHttpServer final : private core::NonCopyableOrMovable {
 public:
  using Handler = std::function<core::Result<HttpServerResponse>(const HttpServerRequest&)>;
  using AsyncHandler = std::function<boost::asio::awaitable<core::Result<EventHttpResponse>>(
      HttpServerRequest, engine::RequestExecutionContext)>;
  explicit EventHttpServer(Handler handler, EventHttpServerOptions options = {});
  explicit EventHttpServer(AsyncHandler handler, EventHttpServerOptions options = {});
  EventHttpServer(std::shared_ptr<engine::AsyncDispatcher> dispatcher, lf::a2a::v1::AgentCard card,
                  const ServerBindingsOptions& bindings, EventHttpServerOptions options = {});
  ~EventHttpServer();
  [[nodiscard]] core::Result<void> Start();
  [[nodiscard]] std::uint16_t port() const noexcept;
  // The resolved socket worker count for this server.
  [[nodiscard]] std::size_t network_workers() const noexcept;
  void Shutdown();
  void Join();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace a2a::server
