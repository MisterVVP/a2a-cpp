// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <memory>
#include <string>

#include "a2a/server/event_http_server.h"
#include "event_http_native_handler.h"

namespace a2a::server::http_runtime {

class NativeGetTaskIngress final {
 public:
  NativeGetTaskIngress() = default;
  NativeGetTaskIngress(boost::asio::any_io_executor executor, std::string rest_api_base_path);

  [[nodiscard]] bool Accepts(const HttpServerRequest& request) const noexcept;
  [[nodiscard]] const boost::asio::any_io_executor& executor() const noexcept { return executor_; }

 private:
  boost::asio::any_io_executor executor_;
  std::string task_prefix_;
};

class EventHttpListener final {
 public:
  EventHttpListener(EventHttpServer::AsyncHandler handler, NativeHttpHandler native_handler,
                    EventHttpServerOptions options, NativeGetTaskIngress ingress = {});
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
