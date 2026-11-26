// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <boost/asio/io_context.hpp>
#include <cstddef>

#include "a2a/server/event_http_dispatcher.h"

namespace a2a::server::http_runtime {

struct NativeHttpBatchLimits final {
  std::size_t payload_bytes = 0;
  std::size_t id_bytes = 0;
};

class NativeHttpStream : public HttpEventStream {
 public:
  [[nodiscard]] virtual NativeHttpBatchLimits BatchLimits() const noexcept { return {}; }
  virtual boost::asio::awaitable<core::Result<void>> Start(HttpServerResponse response, std::string first_event) = 0;
  virtual void Finish(std::string error_event) = 0;
  virtual void FinishEmpty(HttpServerResponse response) = 0;
  virtual void Abort() = 0;
};

using NativeHttpCompletion = std::function<void(core::Result<HttpServerResponse>)>;
using NativeHttpStreamFactory = std::function<std::shared_ptr<NativeHttpStream>()>;
struct NativeHttpWorkQueue final {
  boost::asio::io_context::executor_type executor;
};
using NativeHttpHandler = std::function<void(HttpServerRequest, engine::RequestExecutionContext, NativeHttpCompletion,
                                             NativeHttpStreamFactory, NativeHttpWorkQueue)>;

class EventHttpHandlerFactory final {
 public:
  [[nodiscard]] static EventHttpServer::AsyncHandler Coroutine(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                               lf::a2a::v1::AgentCard card,
                                                               const ServerBindingsOptions& options);
  [[nodiscard]] static NativeHttpHandler Native(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                lf::a2a::v1::AgentCard card, const ServerBindingsOptions& options);
};

}  // namespace a2a::server::http_runtime
