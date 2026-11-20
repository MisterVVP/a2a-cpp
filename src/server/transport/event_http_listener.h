// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <cstddef>
#include <memory>

#include "a2a/server/event_http_server.h"
#include "event_http_native_handler.h"

namespace a2a::server::http_runtime {

class EventHttpListener final {
 public:
  EventHttpListener(EventHttpServer::AsyncHandler handler, NativeHttpHandler native_handler,
                    EventHttpServerOptions options, boost::asio::any_io_executor native_executor = {},
                    std::size_t network_workers = 0);
  ~EventHttpListener();

  [[nodiscard]] core::Result<void> Start();
  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] std::size_t network_workers() const noexcept;
  void Shutdown();
  void Join();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace a2a::server::http_runtime
