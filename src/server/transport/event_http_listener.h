// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "a2a/server/event_http_server.h"
#include "event_http_native_handler.h"

namespace a2a::server::http_runtime {

class EventHttpListener final {
 public:
  EventHttpListener(EventHttpServer::AsyncHandler handler, NativeHttpHandler native_handler,
                    EventHttpServerOptions options);
  ~EventHttpListener();

  [[nodiscard]] core::Result<void> Start();
  [[nodiscard]] std::uint16_t port() const noexcept;
  void Shutdown();
  void Join();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace a2a::server::http_runtime
