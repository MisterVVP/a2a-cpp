// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include "a2a/server/event_http_dispatcher.h"

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

#include "a2a/core/http_utils.h"

namespace a2a::server {
namespace {
namespace asio = boost::asio;
using DispatchResult = core::Result<DispatchResponse>;
constexpr std::string_view kCancelled = "HTTP dispatch cancelled";
constexpr std::string_view kMissingDispatcher = "Event HTTP dispatcher is required";
using StopCallback = std::stop_callback<std::function<void()>>;

// Own no io_context services: native cleanup can outlive the HTTP executor.
template <typename T>
class Mailbox final : public std::enable_shared_from_this<Mailbox<T>> {
 public:
  bool Send(T value, bool replace_buffered = false) {
    std::function<void(core::Result<T>)> waiter;
    {
      std::lock_guard lock(mutex_);
      if (closed_ || (value_.has_value() && !replace_buffered)) {
        return false;
      }
      if (!waiter_) {
        value_ = std::move(value);
        return true;
      }
      waiter = std::move(waiter_);
    }
    waiter(core::Result<T>(std::move(value)));
    return true;
  }
  void Close() {
    std::function<void(core::Result<T>)> waiter;
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
      value_.reset();
      waiter = std::move(waiter_);
    }
    if (waiter) {
      waiter(core::Error::Network(std::string(kCancelled)));
    }
  }
  template <typename CompletionToken>
  auto Receive(CompletionToken&& token) {
    return asio::async_initiate<CompletionToken, void(core::Result<T>)>(
        [self = this->shared_from_this()](auto handler) {
          auto executor = asio::get_associated_executor(handler);
          auto guard = asio::make_work_guard(executor);
          auto completion = std::make_shared<decltype(handler)>(std::move(handler));
          self->Wait([executor, guard = std::move(guard), completion](core::Result<T> result) mutable {
            asio::post(executor, [guard = std::move(guard), completion, result = std::move(result)]() mutable {
              (*completion)(std::move(result));
            });
          });
        },
        token);
  }

 private:
  void Wait(std::function<void(core::Result<T>)> waiter) {
    std::optional<T> result;
    {
      std::lock_guard lock(mutex_);
      if (!closed_) {
        if (value_.has_value()) {
          result.emplace(std::move(value_.value()));
          value_.reset();
        } else if (!waiter_) {
          waiter_ = std::move(waiter);
          return;
        }
      }
    }
    if (result.has_value()) {
      waiter(core::Result<T>(std::move(result.value())));
    } else {
      waiter(core::Error::Network(std::string(kCancelled)));
    }
  }
  std::mutex mutex_;
  std::optional<T> value_;
  std::function<void(core::Result<T>)> waiter_;
  bool closed_ = false;
};
using ReplyChannel = Mailbox<std::shared_ptr<DispatchResult>>;

class EmptySession final : public ServerStreamSession {
 public:
  core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    return std::optional<lf::a2a::v1::StreamResponse>{};
  }
};

struct StreamItem final {
  std::optional<lf::a2a::v1::StreamResponse> event;
  std::optional<core::Error> error;
};

class StreamBridge final : public engine::AsyncServerStreamSession {
 public:
  explicit StreamBridge(std::function<core::Result<std::string>(const lf::a2a::v1::StreamResponse&)> encoder)
      : encode(std::move(encoder)) {}
  asio::awaitable<core::Result<void>> Write(lf::a2a::v1::StreamResponse event) override {
    std::shared_ptr<HttpEventStream> sink;
    {
      std::lock_guard lock(stream_mutex);
      sink = socket_stream;
    }
    if (sink) {
      const auto bytes = encode(event);
      if (!bytes.ok()) {
        co_return bytes.error();
      }
      co_return co_await sink->Write(bytes.value());
    }
    if (!events->Send(StreamItem{.event = std::move(event), .error = {}})) {
      co_return core::Error::Network(std::string(kCancelled));
    }
    auto accepted = co_await demand->Receive(asio::use_awaitable);
    if (!accepted.ok()) {
      co_return accepted.error();
    }
    co_return core::Result<void>{};
  }

  void Attach(std::shared_ptr<HttpEventStream> sink) {
    std::lock_guard lock(stream_mutex);
    socket_stream = std::move(sink);
  }
  void Cancel() {
    events->Close();
    demand->Close();
  }
  void Complete(DispatchResult result) {
    StreamItem terminal;
    if (!result.ok()) {
      terminal.error = result.error();
      demand->Close();
    }
    (void)events->Send(std::move(terminal), true);
  }
  const std::shared_ptr<Mailbox<StreamItem>> events = std::make_shared<Mailbox<StreamItem>>();
  const std::shared_ptr<Mailbox<bool>> demand = std::make_shared<Mailbox<bool>>();

  std::mutex stream_mutex;
  std::shared_ptr<HttpEventStream> socket_stream;
  const std::function<core::Result<std::string>(const lf::a2a::v1::StreamResponse&)> encode;
  std::shared_ptr<engine::RequestExecution> execution;
  std::unique_ptr<StopCallback> cancellation;
};

std::unique_ptr<StopCallback> CancelOnDisconnect(engine::RequestExecutionContext context,
                                                 std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                 std::shared_ptr<engine::RequestExecution> execution,
                                                 std::function<void()> wake) {
  const auto token = context.StopToken();
  return std::make_unique<StopCallback>(token, [context = std::move(context), dispatcher = std::move(dispatcher),
                                                execution = std::move(execution), wake = std::move(wake)] {
    auto reason = engine::RequestCancellationReason::kClientCancelled;
    if (context.Outcome() == engine::RequestOutcome::kServerShutdown) {
      reason = engine::RequestCancellationReason::kServerShutdown;
    } else if (context.Outcome() == engine::RequestOutcome::kDeadlineExceeded) {
      reason = engine::RequestCancellationReason::kDeadlineExceeded;
    }
    dispatcher->Cancel(execution, reason);
    wake();
  });
}

asio::awaitable<core::Result<EventHttpResponse>> Unary(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                       PreparedDispatch<HttpServerResponse> plan,
                                                       engine::RequestExecutionContext context) {
  const auto replies = std::make_shared<ReplyChannel>();
  auto execution =
      dispatcher->Dispatch(std::move(plan.request), std::move(plan.context),
                           [replies](DispatchResult result, const engine::RequestExecutionContext& /*context*/) {
                             (void)replies->Send(std::make_shared<DispatchResult>(std::move(result)));
                           });
  if (!execution.ok()) {
    auto response = plan.complete(execution.error());
    if (!response.ok()) {
      co_return response.error();
    }
    co_return EventHttpResponse{.response = std::move(response.value()), .stream = {}};
  }
  const auto cancellation =
      CancelOnDisconnect(std::move(context), dispatcher, execution.value(), [replies] { replies->Close(); });
  auto result = co_await replies->Receive(asio::use_awaitable);
  if (!result.ok()) {
    co_return result.error();
  }
  auto response = plan.complete(std::move(*result.value()));
  if (!response.ok()) {
    co_return response.error();
  }
  co_return EventHttpResponse{.response = std::move(response.value()), .stream = {}};
}

asio::awaitable<core::Result<void>> Produce(
    std::shared_ptr<StreamBridge> bridge, StreamItem item,
    std::function<core::Result<std::string>(const lf::a2a::v1::StreamResponse&)> encode,
    std::shared_ptr<HttpEventStream> stream, std::shared_ptr<engine::AsyncDispatcher> dispatcher,
    std::function<core::Result<std::string>(const core::Error&)> encode_error) {
  if (item.event) {
    const auto bytes = encode(*item.event);
    auto written = bytes.ok() ? co_await stream->Write(bytes.value()) : core::Result<void>(bytes.error());
    if (!written.ok()) {
      dispatcher->Cancel(bridge->execution);
      bridge->Cancel();
      co_return written.error();
    }
    bridge->Attach(stream);
    (void)bridge->demand->Send(true);
    auto next = co_await bridge->events->Receive(asio::use_awaitable);
    if (!next.ok()) {
      co_return next.error();
    }
    item = std::move(next.value());
  }
  if (item.error) {
    const auto bytes = encode_error(*item.error);
    if (!bytes.ok()) {
      co_return bytes.error();
    }
    co_return co_await stream->Write(bytes.value());
  }
  co_return core::Result<void>{};
}

asio::awaitable<core::Result<EventHttpResponse>> Streaming(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                           PreparedDispatch<HttpServerResponse> plan,
                                                           engine::RequestExecutionContext context) {
  const auto bridge = std::make_shared<StreamBridge>(plan.encode_event);
  const auto execution =
      dispatcher->DispatchStream(std::move(plan.request), std::move(plan.context), bridge,
                                 [bridge](DispatchResult result, const engine::RequestExecutionContext& /*context*/) {
                                   bridge->Complete(std::move(result));
                                 });
  if (!execution.ok()) {
    auto response = plan.complete(execution.error());
    if (!response.ok()) {
      co_return response.error();
    }
    co_return EventHttpResponse{.response = std::move(response.value()), .stream = {}};
  }
  bridge->execution = execution.value();
  bridge->cancellation =
      CancelOnDisconnect(std::move(context), dispatcher, execution.value(), [weak = std::weak_ptr(bridge)] {
        if (const auto bridge = weak.lock()) {
          bridge->Cancel();
        }
      });
  auto next = co_await bridge->events->Receive(asio::use_awaitable);
  if (!next.ok()) {
    co_return next.error();
  }
  auto first = std::move(next.value());
  if (first.error) {
    auto response = plan.complete(*first.error);
    if (!response.ok()) {
      co_return response.error();
    }
    co_return EventHttpResponse{.response = std::move(response.value()), .stream = {}};
  }
  // Reuse the protocol serializer's headers without running its blocking writer.
  auto response = plan.complete(DispatchResponse(std::make_unique<EmptySession>()));
  if (!response.ok()) {
    co_return response.error();
  }
  response.value().stream_writer = {};
  co_return EventHttpResponse{.response = std::move(response.value()),
                              .stream = [bridge, first = std::move(first), encode = std::move(plan.encode_event),
                                         dispatcher, encode_error = std::move(plan.encode_error)](
                                            std::shared_ptr<HttpEventStream> stream,
                                            const engine::RequestExecutionContext& /*context*/) mutable {
                                return Produce(bridge, std::move(first), encode, std::move(stream), dispatcher,
                                               encode_error);
                              }};
}

struct Bindings final {
  Bindings(std::shared_ptr<engine::AsyncDispatcher> execution, lf::a2a::v1::AgentCard card,
           const ServerBindingsOptions& options)
      : dispatcher(std::move(execution)),
        rest(nullptr, std::move(card), RestOptions(options)),
        rpc(nullptr, RpcOptions(options)),
        mux({.normalization_policy = TransportMux::PathNormalizationPolicy::kRootToDefaultPath,
             .default_path = core::http::NormalizeRoutePath(options.jsonrpc.rpc_path)}) {
    mux.RegisterJsonRpcRoute(rpc, {.rpc_path = core::http::NormalizeRoutePath(options.jsonrpc.rpc_path)});
    mux.RegisterRestRoute(rest, {.rest_api_prefix = core::http::NormalizeRoutePath(options.rest.rest_api_base_path)});
  }
  static RestServerTransportOptions RestOptions(const ServerBindingsOptions& options) {
    auto rest = options.rest;
    if (!options.required_extensions.empty()) {
      rest.required_extensions = options.required_extensions;
    }
    return rest;
  }
  static JsonRpcServerTransportOptions RpcOptions(const ServerBindingsOptions& options) {
    auto rpc = options.jsonrpc;
    if (!options.required_extensions.empty()) {
      rpc.required_extensions = options.required_extensions;
    }
    return rpc;
  }
  std::shared_ptr<engine::AsyncDispatcher> dispatcher;
  RestServerTransport rest;
  JsonRpcServerTransport rpc;
  TransportMux mux;
};

asio::awaitable<core::Result<EventHttpResponse>> Handle(std::shared_ptr<Bindings> bindings, HttpServerRequest request,
                                                        engine::RequestExecutionContext context) {
  if (!bindings->dispatcher) {
    co_return core::Error::Validation(std::string(kMissingDispatcher));
  }
  auto prepared = bindings->mux.PrepareRequest(request);
  if (!prepared.ok()) {
    co_return prepared.error();
  }
  if (auto* response = std::get_if<HttpServerResponse>(&prepared.value())) {
    co_return EventHttpResponse{.response = std::move(*response), .stream = {}};
  }
  auto plan = std::get<PreparedDispatch<HttpServerResponse>>(std::move(prepared.value()));
  const auto operation = plan.request.operation;
  if (operation == DispatcherOperation::kSendStreamingMessage || operation == DispatcherOperation::kSubscribeTask) {
    co_return co_await Streaming(bindings->dispatcher, std::move(plan), std::move(context));
  }
  co_return co_await Unary(bindings->dispatcher, std::move(plan), std::move(context));
}
}  // namespace

EventHttpServer::AsyncHandler MakeEventHttpHandler(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                   lf::a2a::v1::AgentCard card, const ServerBindingsOptions& options) {
  const auto bindings = std::make_shared<Bindings>(std::move(dispatcher), std::move(card), options);
  return [bindings](HttpServerRequest request, engine::RequestExecutionContext context) {
    return Handle(bindings, std::move(request), std::move(context));
  };
}
}  // namespace a2a::server
