// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/event_http_server.h"

#include <boost/asio/any_io_executor.hpp>
#include <memory>
#include <utility>

#include "event_http_listener.h"
#include "event_http_native_handler.h"
#include "event_http_server_access.h"

namespace a2a::server {
namespace {

EventHttpServer::AsyncHandler AdaptHandler(EventHttpServer::Handler handler) {
  if (!handler) {
    return {};
  }
  return [handler = std::move(handler)](
             HttpServerRequest request,
             engine::RequestExecutionContext) mutable -> boost::asio::awaitable<core::Result<EventHttpResponse>> {
    auto response = handler(request);
    if (!response.ok()) {
      co_return response.error();
    }
    co_return EventHttpResponse{.response = std::move(response.value()), .stream = {}};
  };
}

}  // namespace

struct EventHttpServer::Impl final {
  explicit Impl(std::unique_ptr<http_runtime::EventHttpListener> listener) : listener(std::move(listener)) {}

  std::unique_ptr<http_runtime::EventHttpListener> listener;
};

EventHttpServer::EventHttpServer(Handler handler, EventHttpServerOptions options)
    : EventHttpServer(AdaptHandler(std::move(handler)), std::move(options)) {}

EventHttpServer::EventHttpServer(AsyncHandler handler, EventHttpServerOptions options)
    : impl_(std::make_unique<Impl>(std::make_unique<http_runtime::EventHttpListener>(
          std::move(handler), http_runtime::NativeHttpHandler{}, std::move(options)))) {}

EventHttpServer::EventHttpServer(std::shared_ptr<engine::AsyncDispatcher> dispatcher, lf::a2a::v1::AgentCard card,
                                 const ServerBindingsOptions& bindings, EventHttpServerOptions options)
    : EventHttpServer(std::move(dispatcher), std::move(card), bindings, std::move(options), 0) {}

EventHttpServer::EventHttpServer(std::shared_ptr<engine::AsyncDispatcher> dispatcher, lf::a2a::v1::AgentCard card,
                                 const ServerBindingsOptions& bindings, EventHttpServerOptions options,
                                 std::size_t network_workers) {
  auto native_executor = dispatcher ? dispatcher->NativeHttpIngressExecutor() : boost::asio::any_io_executor{};
  auto handler = http_runtime::EventHttpHandlerFactory::Native(std::move(dispatcher), std::move(card), bindings);
  impl_ = std::make_unique<Impl>(std::make_unique<http_runtime::EventHttpListener>(
      AsyncHandler{}, std::move(handler), std::move(options), std::move(native_executor), network_workers));
}

std::size_t http_runtime::EventHttpServerAccess::NetworkWorkers(const EventHttpServer& server) noexcept {
  return server.impl_->listener->network_workers();
}

EventHttpServer::~EventHttpServer() = default;

core::Result<void> EventHttpServer::Start() { return impl_->listener->Start(); }

std::uint16_t EventHttpServer::port() const noexcept { return impl_->listener->port(); }

void EventHttpServer::Shutdown() { impl_->listener->Shutdown(); }

void EventHttpServer::Join() { impl_->listener->Join(); }

}  // namespace a2a::server
