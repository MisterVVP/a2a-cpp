// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "a2a/server/event_http_dispatcher.h"

namespace a2a::server::http_runtime {

class NativeHttpStream : public HttpEventStream {
 public:
  virtual boost::asio::awaitable<core::Result<void>> Start(HttpServerResponse response, std::string first_event) = 0;
  virtual void Finish(std::string error_event) = 0;
  virtual void FinishEmpty(HttpServerResponse response) = 0;
  virtual void Abort() = 0;
};

using NativeHttpCompletion = std::function<void(core::Result<HttpServerResponse>)>;
using NativeHttpStreamFactory = std::function<std::shared_ptr<NativeHttpStream>()>;
using NativeHttpHandler = std::function<void(HttpServerRequest, engine::RequestExecutionContext, NativeHttpCompletion,
                                             NativeHttpStreamFactory)>;

class EventHttpHandlerFactory final {
 public:
  [[nodiscard]] static EventHttpServer::AsyncHandler Coroutine(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                               lf::a2a::v1::AgentCard card,
                                                               const ServerBindingsOptions& options);
  [[nodiscard]] static NativeHttpHandler Native(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                lf::a2a::v1::AgentCard card, const ServerBindingsOptions& options);
};

}  // namespace a2a::server::http_runtime
