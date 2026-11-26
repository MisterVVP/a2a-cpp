// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include "event_http_native_handler.h"

#include <google/protobuf/struct.pb.h>

#include <algorithm>
#include <atomic>
#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <cstddef>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "a2a/core/http_utils.h"
#include "a2a/server/event_http_dispatcher.h"

namespace a2a::server::http_runtime {
namespace {
namespace asio = boost::asio;
using DispatchResult = core::Result<DispatchResponse>;
constexpr std::string_view kCancelled = "HTTP dispatch cancelled";
constexpr std::string_view kMissingDispatcher = "Event HTTP dispatcher is required";
using StopCallback = std::stop_callback<std::function<void()>>;

// Own no io_context services: native cleanup can outlive the HTTP executor.
template <typename T>
class Mailbox final : public std::enable_shared_from_this<Mailbox<T>> {
  using Completion = asio::any_completion_handler<void(core::Result<T>)>;

 public:
  bool Send(T value, bool replace_buffered = false) {
    Completion waiter;
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
    Completion waiter;
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
          self->Wait(
              [executor, guard = std::move(guard), handler = std::move(handler)](core::Result<T> result) mutable {
                asio::post(executor, [guard = std::move(guard), handler = std::move(handler),
                                      result = std::move(result)]() mutable { handler(std::move(result)); });
              });
        },
        token);
  }

 private:
  void Wait(Completion waiter) {
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
  Completion waiter_;
  bool closed_ = false;
};
using ReplyChannel = Mailbox<DispatchResult>;

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
      auto bytes = encode(event);
      if (!bytes.ok()) {
        co_return bytes.error();
      }
      co_return co_await sink->Write(std::move(bytes.value()));
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
                             (void)replies->Send(std::move(result));
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
  auto response = plan.complete(std::move(result.value()));
  if (!response.ok()) {
    co_return response.error();
  }
  co_return EventHttpResponse{.response = std::move(response.value()), .stream = {}};
}

asio::awaitable<core::Result<void>> Produce(std::shared_ptr<StreamBridge> bridge, bool initial_event,
                                            std::shared_ptr<HttpEventStream> stream,
                                            std::function<core::Result<std::string>(const core::Error&)> encode_error) {
  StreamItem item;
  if (initial_event) {
    bridge->Attach(stream);
    (void)bridge->demand->Send(true);
    auto next = co_await bridge->events->Receive(asio::use_awaitable);
    if (!next.ok()) {
      co_return next.error();
    }
    item = std::move(next.value());
  }
  if (item.error) {
    auto bytes = encode_error(*item.error);
    if (!bytes.ok()) {
      co_return bytes.error();
    }
    co_return co_await stream->Write(std::move(bytes.value()));
  }
  co_return core::Result<void>{};
}

asio::awaitable<core::Result<EventHttpResponse>> Streaming(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                           PreparedDispatch<HttpServerResponse> plan,
                                                           engine::RequestExecutionContext context) {
  const auto bridge = std::make_shared<StreamBridge>(std::move(plan.encode_event));
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
  auto response = plan.complete(DispatchResponse(std::make_unique<EmptySession>()));
  if (!response.ok()) {
    co_return response.error();
  }
  response.value().stream_writer = {};
  const bool initial_event = first.event.has_value();
  if (initial_event) {
    auto bytes = bridge->encode(*first.event);
    if (!bytes.ok()) {
      dispatcher->Cancel(bridge->execution);
      bridge->Cancel();
      co_return bytes.error();
    }
    response.value().body = std::move(bytes.value());
  }
  co_return EventHttpResponse{.response = std::move(response.value()),
                              .stream = [bridge, initial_event, encode_error = std::move(plan.encode_error)](
                                            std::shared_ptr<HttpEventStream> stream,
                                            const engine::RequestExecutionContext& /*context*/) mutable {
                                return Produce(bridge, initial_event, std::move(stream), encode_error);
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

constexpr std::size_t kInlineResponseBudget = 4096;
constexpr std::size_t kInlineObjectCost = 32;
constexpr std::size_t kInlineErrorBytes = 512;

class InlineEncodingBudget final {
 public:
  bool Fits(const lf::a2a::v1::Task& task) {
    return Charge(kInlineObjectCost) && Charge(task.id().size()) && Charge(task.context_id().size()) &&
           (!task.status().has_message() || Fits(task.status().message())) && FitsAll(task.artifacts()) &&
           FitsAll(task.history()) && Fits(task.metadata());
  }

  bool Fits(const lf::a2a::v1::Message& message) {
    return Charge(kInlineObjectCost) && Charge(message.message_id().size()) && Charge(message.context_id().size()) &&
           Charge(message.task_id().size()) && FitsAll(message.parts()) && Fits(message.metadata()) &&
           FitsAll(message.extensions()) && FitsAll(message.reference_task_ids());
  }

  bool Fits(const ListTasksResponse& response) {
    return Charge(response.next_page_token.size()) && FitsAll(response.tasks);
  }

 private:
  template <typename Range>
  bool FitsAll(const Range& values) {
    return std::ranges::all_of(values, [this](const auto& value) { return this->Fits(value); });
  }

  bool Fits(const lf::a2a::v1::Artifact& artifact) {
    return Charge(kInlineObjectCost) && Charge(artifact.artifact_id().size()) && Charge(artifact.name().size()) &&
           Charge(artifact.description().size()) && FitsAll(artifact.parts()) && Fits(artifact.metadata()) &&
           FitsAll(artifact.extensions());
  }

  bool Fits(const lf::a2a::v1::Part& part) {
    return Charge(kInlineObjectCost) && Charge(part.text().size()) && Charge(part.url().size()) &&
           Charge(part.raw().size()) && Charge(part.filename().size()) && Charge(part.media_type().size()) &&
           Fits(part.metadata()) && (!part.has_data() || Fits(part.data()));
  }

  bool Fits(const std::string& text) { return Charge(kInlineObjectCost) && Charge(text.size()); }

  bool Fits(const google::protobuf::Struct& metadata) {
    return std::ranges::all_of(metadata.fields(),
                               [this](const auto& entry) { return Charge(entry.first.size()) && Fits(entry.second); });
  }

  bool Fits(const google::protobuf::Value& value) {
    if (!Charge(kInlineObjectCost)) {
      return false;
    }
    switch (value.kind_case()) {
      case google::protobuf::Value::kStructValue:
        return Fits(value.struct_value());
      case google::protobuf::Value::kListValue:
        return FitsAll(value.list_value().values());
      case google::protobuf::Value::kStringValue:
        return Charge(value.string_value().size());
      default:
        return true;
    }
  }

  bool Charge(std::size_t cost) {
    if (cost > remaining_) {
      return false;
    }
    remaining_ -= cost;
    return true;
  }

  std::size_t remaining_ = kInlineResponseBudget;
};

bool CanEncodeInline(const DispatchResult& result) {
  if (!result.ok()) {
    return result.error().message().size() <= kInlineErrorBytes;
  }
  const auto& payload = result.value().payload();
  InlineEncodingBudget budget;
  if (const auto* task = std::get_if<lf::a2a::v1::Task>(&payload)) {
    return budget.Fits(*task);
  }
  if (const auto* sent = std::get_if<lf::a2a::v1::SendMessageResponse>(&payload)) {
    return sent->has_task() ? budget.Fits(sent->task()) : sent->has_message() && budget.Fits(sent->message());
  }
  if (const auto* listed = std::get_if<ListTasksResponse>(&payload)) {
    return budget.Fits(*listed);
  }
  return std::holds_alternative<std::monostate>(payload);
}

class NativeReply final : public std::enable_shared_from_this<NativeReply>, private core::NonCopyableOrMovable {
 public:
  NativeReply(PreparedDispatch<HttpServerResponse>& plan, NativeHttpCompletion done,
              std::shared_ptr<std::atomic_size_t> active_unary = {})
      : serialize_(std::move(plan.complete)), done_(std::move(done)), active_unary_(std::move(active_unary)) {
    if (active_unary_) {
      active_unary_->fetch_add(1, std::memory_order_relaxed);
    }
  }

  ~NativeReply() { ReleaseUnary(); }

  void Deliver(DispatchResult result) {
    const auto done = Claim();
    if (done) {
      Complete(done, Encode(std::move(result)));
    }
  }

  void Schedule(DispatchResult result, const NativeHttpWorkQueue& work) {
    const auto done = Claim();
    if (!done) {
      return;
    }
    if (active_unary_->load(std::memory_order_relaxed) == 1 && CanEncodeInline(result)) {
      Complete(done, Encode(std::move(result)));
      return;
    }
    try {
      asio::post(work.executor, [self = shared_from_this(), done, result = std::move(result)]() mutable {
        self->Complete(done, self->Encode(std::move(result)));
      });
    } catch (...) {
      Complete(done, core::Error::Internal(kDispatchResponseEncodingFailed));
    }
  }

  void Cancel() {
    auto done = Claim();
    if (done) {
      Complete(done, core::Error::Network(std::string(kCancelled)));
    }
  }

  void ObserveDisconnect(const std::shared_ptr<engine::AsyncDispatcher>& dispatcher,
                         std::shared_ptr<engine::RequestExecution> execution, engine::RequestExecutionContext context) {
    cancellation_ = CancelOnDisconnect(std::move(context), dispatcher, std::move(execution), [weak = weak_from_this()] {
      if (const auto reply = weak.lock()) {
        reply->Cancel();
      }
    });
  }

  NativeHttpCompletion Claim() {
    std::lock_guard lock(mutex_);
    return std::exchange(done_, {});
  }

 private:
  static constexpr auto kDispatchResponseEncodingFailed = "HTTP dispatch response encoding failed";

  core::Result<HttpServerResponse> Encode(DispatchResult result) {
    try {
      return serialize_(std::move(result));
    } catch (...) {
      return core::Error::Internal(kDispatchResponseEncodingFailed);
    }
  }

  void Complete(const NativeHttpCompletion& done, core::Result<HttpServerResponse> result) {
    ReleaseUnary();
    done(std::move(result));
  }

  void ReleaseUnary() noexcept {
    if (const auto active = std::move(active_unary_)) {
      active->fetch_sub(1, std::memory_order_relaxed);
    }
  }

  decltype(PreparedDispatch<HttpServerResponse>::complete) serialize_;
  std::mutex mutex_;
  NativeHttpCompletion done_;
  std::shared_ptr<std::atomic_size_t> active_unary_;
  std::unique_ptr<StopCallback> cancellation_;
};

void DirectUnary(const std::shared_ptr<engine::AsyncDispatcher>& dispatcher, PreparedDispatch<HttpServerResponse> plan,
                 engine::RequestExecutionContext context, NativeHttpCompletion done, const NativeHttpWorkQueue& work,
                 const std::shared_ptr<std::atomic_size_t>& active_unary) {
  const auto reply = std::make_shared<NativeReply>(plan, std::move(done), active_unary);
  auto execution = dispatcher->Dispatch(std::move(plan.request), std::move(plan.context),
                                        [reply, work](DispatchResult result, const engine::RequestExecutionContext&) {
                                          reply->Schedule(std::move(result), work);
                                        });
  if (!execution.ok()) {
    reply->Deliver(execution.error());
    return;
  }
  reply->ObserveDisconnect(dispatcher, std::move(execution.value()), std::move(context));
}

bool IsCompactStatusEvent(const lf::a2a::v1::StreamResponse& event, std::size_t max_id_bytes) {
  const auto& status = event.status_update();
  return event.has_status_update() && status.task_id().size() <= max_id_bytes &&
         status.context_id().size() <= max_id_bytes && !status.has_metadata() && !status.status().has_message();
}

class DirectStream final : public engine::AsyncServerStreamSession {
 public:
  DirectStream(PreparedDispatch<HttpServerResponse>& plan, NativeHttpCompletion done,
               std::shared_ptr<NativeHttpStream> stream)
      : head_(plan.complete(DispatchResponse(std::make_unique<EmptySession>()))),
        reply_(std::make_shared<NativeReply>(plan, std::move(done))),
        encode_(std::move(plan.encode_event)),
        encode_error_(std::move(plan.encode_error)),
        stream_(std::move(stream)) {}

  asio::awaitable<core::Result<void>> Write(lf::a2a::v1::StreamResponse event) override {
    auto frame = encode_(event);
    if (!frame.ok()) {
      co_return frame.error();
    }
    co_return co_await WriteFrame(std::move(frame.value()));
  }

  asio::awaitable<core::Result<void>> WriteBatch(std::vector<lf::a2a::v1::StreamResponse> events) override {
    const auto limits = stream_->BatchLimits();
    if (limits.payload_bytes == 0 || events.size() <= 1 || events.size() > kMaxBatchEvents ||
        !std::ranges::all_of(events, [max_id_bytes = limits.id_bytes](const auto& event) {
          return IsCompactStatusEvent(event, max_id_bytes);
        })) {
      return AsyncServerStreamSession::WriteBatch(std::move(events));
    }
    std::string frames;
    for (const auto& event : events) {
      auto frame = encode_(event);
      if (!frame.ok() || frame.value().size() > limits.payload_bytes - frames.size()) {
        return AsyncServerStreamSession::WriteBatch(std::move(events));
      }
      frames.append(frame.value());
    }
    return WriteFrame(std::move(frames));
  }

  void Complete(DispatchResult result) {
    if (!started_ && (!result.ok() || !head_.ok())) {
      reply_->Deliver(result.ok() ? DispatchResult(head_.error()) : std::move(result));
      return;
    }
    const auto done = reply_->Claim();
    if (!done) {
      return;
    }
    try {
      if (!started_) {
        stream_->FinishEmpty(std::move(head_.value()));
      } else if (result.ok()) {
        stream_->Finish({});
      } else {
        auto error = encode_error_(result.error());
        if (error.ok()) {
          stream_->Finish(std::move(error.value()));
        } else {
          stream_->Abort();
        }
      }
    } catch (...) {
      stream_->Abort();
    }
  }

  void ObserveDisconnect(const std::shared_ptr<engine::AsyncDispatcher>& dispatcher,
                         std::shared_ptr<engine::RequestExecution> execution, engine::RequestExecutionContext context) {
    reply_->ObserveDisconnect(dispatcher, std::move(execution), std::move(context));
  }

 private:
  asio::awaitable<core::Result<void>> WriteFrame(std::string bytes) {
    if (started_) {
      return stream_->Write(std::move(bytes));
    }
    if (!head_.ok()) {
      return WriteError(head_.error());
    }
    started_ = true;
    return stream_->Start(std::move(head_.value()), std::move(bytes));
  }
  static asio::awaitable<core::Result<void>> WriteError(core::Error error) { co_return std::move(error); }

  core::Result<HttpServerResponse> head_;
  std::shared_ptr<NativeReply> reply_;
  decltype(PreparedDispatch<HttpServerResponse>::encode_event) encode_;
  decltype(PreparedDispatch<HttpServerResponse>::encode_error) encode_error_;
  std::shared_ptr<NativeHttpStream> stream_;
  bool started_ = false;
};

}  // namespace

EventHttpServer::AsyncHandler EventHttpHandlerFactory::Coroutine(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                                 lf::a2a::v1::AgentCard card,
                                                                 const ServerBindingsOptions& options) {
  const auto bindings = std::make_shared<Bindings>(std::move(dispatcher), std::move(card), options);
  return [bindings](HttpServerRequest request, engine::RequestExecutionContext context) {
    return Handle(bindings, std::move(request), std::move(context));
  };
}
NativeHttpHandler EventHttpHandlerFactory::Native(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                  lf::a2a::v1::AgentCard card, const ServerBindingsOptions& options) {
  if (!dispatcher) {
    return {};
  }
  const auto bindings = std::make_shared<Bindings>(std::move(dispatcher), std::move(card), options);
  auto active_unary = std::make_shared<std::atomic_size_t>(0);
  return [bindings, active_unary = std::move(active_unary)](
             const HttpServerRequest& request, engine::RequestExecutionContext context, NativeHttpCompletion done,
             const NativeHttpStreamFactory& stream_factory, const NativeHttpWorkQueue& work) {
    auto prepared = bindings->mux.PrepareRequest(request);
    if (!prepared.ok()) {
      done(prepared.error());
      return;
    }
    if (auto* response = std::get_if<HttpServerResponse>(&prepared.value())) {
      done(std::move(*response));
      return;
    }
    auto plan = std::get<PreparedDispatch<HttpServerResponse>>(std::move(prepared.value()));
    if (plan.request.operation == DispatcherOperation::kSendStreamingMessage ||
        plan.request.operation == DispatcherOperation::kSubscribeTask) {
      const auto stream = std::make_shared<DirectStream>(plan, std::move(done), stream_factory());
      auto execution =
          bindings->dispatcher->DispatchStream(std::move(plan.request), std::move(plan.context), stream,
                                               [stream](DispatchResult result, const engine::RequestExecutionContext&) {
                                                 stream->Complete(std::move(result));
                                               });
      if (execution.ok()) {
        stream->ObserveDisconnect(bindings->dispatcher, std::move(execution.value()), std::move(context));
      } else {
        stream->Complete(execution.error());
      }
      return;
    }
    DirectUnary(bindings->dispatcher, std::move(plan), std::move(context), std::move(done), work, active_unary);
  };
}
}  // namespace a2a::server::http_runtime

namespace a2a::server {
EventHttpServer::AsyncHandler MakeEventHttpHandler(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                   lf::a2a::v1::AgentCard card, const ServerBindingsOptions& options) {
  return http_runtime::EventHttpHandlerFactory::Coroutine(std::move(dispatcher), std::move(card), options);
}
}  // namespace a2a::server
