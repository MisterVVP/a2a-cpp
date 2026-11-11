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

namespace a2a::server {

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
};

// Optional a2a::asio listener. Linux epoll, macOS kqueue, Windows IOCP through
// Boost.Asio. Exactly one network thread; application code always runs on the
// fixed application pool. Configure before Start; Start is single-use.
// Shutdown is thread-safe and non-blocking. Join/destruction must run outside
// callbacks, drain cancellation and wait for legacy blocking work to return.
class EventHttpServer final : private core::NonCopyableOrMovable {
 public:
  using Handler = std::function<core::Result<HttpServerResponse>(const HttpServerRequest&)>;
  using AsyncHandler = std::function<boost::asio::awaitable<core::Result<EventHttpResponse>>(
      HttpServerRequest, engine::RequestExecutionContext)>;
  explicit EventHttpServer(Handler handler, EventHttpServerOptions options = {});
  explicit EventHttpServer(AsyncHandler handler, EventHttpServerOptions options = {});
  ~EventHttpServer();
  [[nodiscard]] core::Result<void> Start();
  [[nodiscard]] std::uint16_t port() const noexcept;
  void Shutdown();
  void Join();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace a2a::server
