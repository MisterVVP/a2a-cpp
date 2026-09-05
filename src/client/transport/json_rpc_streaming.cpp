// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/json_rpc_transport.h"

#include "json_rpc_transport_internal.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "a2a/client/sse_parser.h"
#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/json_rpc.h"
#include "a2a/core/json_value.h"
#include "a2a/core/protojson.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "a2a/core/task_states.h"
#include "core/subscription_diagnostics.h"
#endif
#include "a2a/core/version.h"
#include "a2a/http/http_client.h"

namespace a2a::client {
namespace {

using json_rpc_internal::BuildJsonRpcEnvelope;
using json_rpc_internal::BuildJsonRpcEnvelopeError;
using json_rpc_internal::JoinUrl;
using json_rpc_internal::ParseResponseResult;
using json_rpc_internal::ParseResultMessage;
using json_rpc_internal::ToClientHttpResponse;
using json_rpc_internal::ToSharedHttpRequest;
using json_rpc_internal::ValidateResponseVersion;

constexpr std::string_view kEmptyJsonObject = "{}";
constexpr std::string_view kStreamingSuccessRequiresSseMessage =
    "JSON-RPC streaming success response must use text/event-stream";
constexpr char kJsonRpcTransportName[] = "jsonrpc";
constexpr char kJsonRpcTransportShuttingDownMessage[] = "JSON-RPC transport is shutting down";
constexpr char kJsonRpcStreamRequesterNotConfiguredMessage[] = "HTTP stream requester is not configured";
constexpr char kJsonRpcStreamContentTypeMessage[] = "JSON-RPC stream response must use text/event-stream";
constexpr char kJsonRpcStreamMetadataOrderMessage[] = "JSON-RPC stream metadata must be validated before body chunks";
constexpr std::string_view kStreamStatusErrorMessage = "JSON-RPC stream returned non-success HTTP status";
constexpr std::string_view kJsonRpcInterfaceRequiredMessage = "JsonRpcTransport requires a JSON-RPC interface";
constexpr std::string_view kJsonRpcUrlRequiredMessage = "Resolved JSON-RPC interface URL is required";
constexpr std::string_view kEmptyRequestIdMessage = "JSON-RPC request id generator returned an empty id";
constexpr std::string_view kTaskIdRequiredMessage = "GetTaskRequest.id is required";

StreamCancellationRegistrar MakeStreamCancellationRegistrar(const std::shared_ptr<StreamHandle::State>& state) {
  return [weak_state = std::weak_ptr<StreamHandle::State>(state)](const std::function<void()>& callback) {
    if (const auto locked_state = weak_state.lock()) {
      locked_state->RegisterCancelCallback(callback);
    }
  };
}

template <typename T>
core::Result<T> ParseTypedResultPayload(std::string_view response_body, int response_status_code,
                                        std::string_view expected_id) {
  const auto range = core::json::FindTopLevelObjectMemberValue(response_body, core::json_rpc::kResultMemberName);
  if (!range.has_value()) {
    const HttpClientResponse response{
        .status_code = response_status_code, .headers = {}, .body = std::string(response_body)};
    const auto result = ParseResponseResult(response, expected_id);
    if (!result.ok()) {
      return result.error();
    }
    return ParseResultMessage<T>(result.value(), response_status_code);
  }

  const std::string_view prefix = response_body.substr(0, range->begin);
  const std::string_view suffix = response_body.substr(range->end);
  std::string validation_body;
  validation_body.reserve(prefix.size() + kEmptyJsonObject.size() + suffix.size());
  validation_body.append(prefix);
  validation_body.append(kEmptyJsonObject);
  validation_body.append(suffix);
  const HttpClientResponse validation_response{
      .status_code = response_status_code, .headers = {}, .body = std::move(validation_body)};
  const auto validated = ParseResponseResult(validation_response, expected_id);
  if (!validated.ok()) {
    return validated.error();
  }

  T message;
  const std::string_view result_json = response_body.substr(range->begin, range->end - range->begin);
  const auto parsed = core::JsonToMessage(result_json, &message);
  if (!parsed.ok()) {
    return parsed.error().WithTransport(kJsonRpcTransportName).WithHttpStatus(response_status_code);
  }
  return message;
}

bool HasSseContentType(const HeaderMap& headers) {
  const auto content_type = core::http::FindHeaderValue(headers, core::http::kContentTypeHeaderName);
  return content_type.has_value() && core::http::IsSseContentType(content_type.value());
}

bool HasJsonContentType(const HeaderMap& headers) {
  const auto content_type = core::http::FindHeaderValue(headers, core::http::kContentTypeHeaderName);
  return content_type.has_value() && core::http::IsJsonContentType(content_type.value());
}

core::Error BuildJsonRpcStreamStatusError(const HttpClientResponse& response) {
  return core::Error::RemoteProtocol(std::string(kStreamStatusErrorMessage))
      .WithTransport(kJsonRpcTransportName)
      .WithHttpStatus(response.status_code);
}

void MarkInactive(StreamHandle::State& state) { state.active.store(false); }

void NotifyErrorAndStop(StreamHandle::State& state, StreamObserver& observer, const core::Error& error) {
  observer.OnError(error);
  MarkInactive(state);
}

core::Result<void> DispatchJsonRpcSseEvent(const SseEvent& event, std::string_view request_id,
                                           const HttpClientResponse& response, StreamObserver& observer) {
  const auto parsed =
      ParseTypedResultPayload<lf::a2a::v1::StreamResponse>(event.data, response.status_code, request_id);
  if (!parsed.ok()) {
    return parsed.error();
  }
  const auto& response_event = parsed.value();
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  const bool terminal =
      response_event.has_status_update() && core::IsTerminalTaskState(response_event.status_update().status().state());
  {
    const core::subscription_diagnostics::ScopedTimer observation_timer(
        core::subscription_diagnostics::Phase::kClientTerminalObserverCallback, terminal);
    observer.OnEvent(response_event);
  }
#else
  observer.OnEvent(response_event);
#endif
  return {};
}

class JsonRpcSseSession final {
 public:
  JsonRpcSseSession(HttpStreamRequester stream_requester, HttpRequest http_request, StreamObserver& observer,
                    std::shared_ptr<StreamHandle::State> state, std::string request_id)
      : stream_requester_(std::move(stream_requester)),
        http_request_(std::move(http_request)),
        observer_(observer),
        state_(std::move(state)),
        request_id_(std::move(request_id)) {}

  JsonRpcSseSession(HttpStreamRequesterWithCancellation stream_requester, HttpRequest http_request,
                    StreamObserver& observer, std::shared_ptr<StreamHandle::State> state, std::string request_id)
      : cancellable_stream_requester_(std::move(stream_requester)),
        http_request_(std::move(http_request)),
        observer_(observer),
        state_(std::move(state)),
        request_id_(std::move(request_id)) {}

  void Run() {
    const auto metadata_handler = [this](const HttpClientResponse& response) { return ValidateMetadata(response); };
    const auto chunk_handler = [this](std::string_view chunk) { return HandleChunk(chunk); };
    const auto cancelled = [this]() { return state_->cancel_requested.load(); };
    const auto stream_response = cancellable_stream_requester_
                                     ? cancellable_stream_requester_(http_request_, metadata_handler, chunk_handler,
                                                                     cancelled, MakeStreamCancellationRegistrar(state_))
                                     : stream_requester_(http_request_, metadata_handler, chunk_handler, cancelled);
    Complete(stream_response);
  }

  void Complete(const core::Result<HttpClientResponse>& stream_response) {
    if (StopIfCancelled()) {
      return;
    }
    if (!stream_response.ok()) {
      NotifyErrorAndStop(*state_, observer_, stream_response.error());
      return;
    }

    const auto metadata = EnsureMetadata(stream_response.value());
    if (!metadata.ok()) {
      NotifyErrorAndStop(*state_, observer_, metadata.error());
      return;
    }
    if (is_json_response_) {
      HandleJsonResponse(stream_response.value());
      return;
    }

    const auto finish = parser_.Finish([this](const SseEvent& event) { return DispatchEvent(event); });
    if (StopIfCancelled()) {
      return;
    }
    if (!finish.ok()) {
      NotifyErrorAndStop(*state_, observer_, finish.error());
      return;
    }
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    {
      const core::subscription_diagnostics::ScopedTimer completion_timer(
          core::subscription_diagnostics::Phase::kClientCompletionCallback);
      observer_.OnCompleted();
    }
#else
    observer_.OnCompleted();
#endif
    MarkInactive(*state_);
  }

  core::Result<void> ValidateMetadata(const HttpClientResponse& response) {
    response_metadata_ = response;
    const auto version_check = ValidateResponseVersion(response_metadata_);
    if (!version_check.ok()) {
      return version_check.error();
    }
    if (HasJsonContentType(response_metadata_.headers)) {
      is_json_response_ = true;
      metadata_validated_ = true;
      return {};
    }
    if (response_metadata_.status_code < core::http::kSuccessStatusMin ||
        response_metadata_.status_code > core::http::kSuccessStatusMax) {
      return BuildJsonRpcStreamStatusError(response_metadata_);
    }
    if (HasSseContentType(response_metadata_.headers)) {
      metadata_validated_ = true;
      return {};
    }
    return core::Error::RemoteProtocol(kJsonRpcStreamContentTypeMessage)
        .WithTransport(kJsonRpcTransportName)
        .WithHttpStatus(response_metadata_.status_code);
  }

  core::Result<void> EnsureMetadata(const HttpClientResponse& response) {
    if (metadata_validated_) {
      return {};
    }
    return ValidateMetadata(response);
  }

  core::Result<void> HandleChunk(std::string_view chunk) {
    if (state_->cancel_requested.load()) {
      return {};
    }
    if (!metadata_validated_) {
      return core::Error::RemoteProtocol(kJsonRpcStreamMetadataOrderMessage).WithTransport(kJsonRpcTransportName);
    }
    if (is_json_response_) {
      json_response_body_.append(chunk);
      return {};
    }
    return parser_.Feed(chunk, [this](const SseEvent& event) { return DispatchEvent(event); });
  }

  core::Result<void> DispatchEvent(const SseEvent& event) {
    if (state_->cancel_requested.load()) {
      return {};
    }
    return DispatchJsonRpcSseEvent(event, request_id_, response_metadata_, observer_);
  }

  bool StopIfCancelled() {
    if (!state_->cancel_requested.load()) {
      return false;
    }
    MarkInactive(*state_);
    return true;
  }

  void HandleJsonResponse(const HttpClientResponse& stream_response) {
    if (json_response_body_.empty()) {
      json_response_body_ = stream_response.body;
    }
    HttpClientResponse json_response = response_metadata_;
    json_response.body = std::move(json_response_body_);
    const auto result = ParseResponseResult(json_response, request_id_);
    if (!result.ok()) {
      NotifyErrorAndStop(*state_, observer_, result.error());
      return;
    }
    if (json_response.status_code < core::http::kSuccessStatusMin ||
        json_response.status_code > core::http::kSuccessStatusMax) {
      NotifyErrorAndStop(*state_, observer_, BuildJsonRpcStreamStatusError(json_response));
      return;
    }
    NotifyErrorAndStop(*state_, observer_,
                       BuildJsonRpcEnvelopeError(kStreamingSuccessRequiresSseMessage, json_response));
  }

  HttpStreamRequester stream_requester_;
  HttpStreamRequesterWithCancellation cancellable_stream_requester_;
  HttpRequest http_request_;
  StreamObserver& observer_;
  std::shared_ptr<StreamHandle::State> state_;
  std::string request_id_;
  SseParser parser_;
  HttpClientResponse response_metadata_;
  std::string json_response_body_;
  bool metadata_validated_ = false;
  bool is_json_response_ = false;
};

core::Result<void> HandleAsyncJsonRpcMetadata(const std::shared_ptr<JsonRpcSseSession>& session,
                                              const std::shared_ptr<StreamHandle::State>& state,
                                              const std::shared_ptr<std::atomic<bool>>& shutdown,
                                              const a2a::http::Response& metadata) {
  StreamHandle::State::CallbackExecutionScope callback_scope(*state);
  if (shutdown->load()) {
    return core::Error::Network(kJsonRpcTransportShuttingDownMessage).WithTransport(kJsonRpcTransportName);
  }
  return session->ValidateMetadata(ToClientHttpResponse(
      a2a::http::Response{.status_code = metadata.status_code, .headers = metadata.headers, .body = metadata.body}));
}

core::Result<void> HandleAsyncJsonRpcChunk(const std::shared_ptr<JsonRpcSseSession>& session,
                                           const std::shared_ptr<StreamHandle::State>& state,
                                           const std::shared_ptr<std::atomic<bool>>& shutdown, std::string_view chunk) {
  StreamHandle::State::CallbackExecutionScope callback_scope(*state);
  if (shutdown->load()) {
    return core::Error::Network(kJsonRpcTransportShuttingDownMessage).WithTransport(kJsonRpcTransportName);
  }
  return session->HandleChunk(chunk);
}

void CompleteAsyncJsonRpcStream(const std::shared_ptr<JsonRpcSseSession>& session,
                                const std::shared_ptr<StreamHandle::State>& state,
                                const std::shared_ptr<std::atomic<bool>>& shutdown,
                                core::Result<a2a::http::Response> response) {
  StreamHandle::State::CallbackExecutionScope callback_scope(*state);
  if (!shutdown->load()) {
    if (!response.ok()) {
      session->Complete(response.error());
    } else {
      session->Complete(ToClientHttpResponse(std::move(response.value())));
    }
  }
  {
    std::lock_guard lock(state->completion_mutex);
    state->completed = true;
  }
  state->completion_condition.notify_all();
}

void RunJsonRpcSseWorker(const HttpStreamRequester& stream_requester, HttpRequest http_request,
                         StreamObserver& observer, const std::shared_ptr<StreamHandle::State>& state,
                         std::string request_id) {
  JsonRpcSseSession session(stream_requester, std::move(http_request), observer, state, std::move(request_id));
  session.Run();
}

void RunJsonRpcSseWorker(const HttpStreamRequesterWithCancellation& stream_requester, HttpRequest http_request,
                         StreamObserver& observer, const std::shared_ptr<StreamHandle::State>& state,
                         std::string request_id) {
  JsonRpcSseSession session(stream_requester, std::move(http_request), observer, state, std::move(request_id));
  session.Run();
}


}  // namespace

core::Result<std::unique_ptr<StreamHandle>> JsonRpcTransport::StartSseStream(std::string_view method_name,
                                                                             const google::protobuf::Message& request,
                                                                             StreamObserver& observer,
                                                                             const CallOptions& options) const {
  if (async_shutdown_->load()) {
    return core::Error::Network(kJsonRpcTransportShuttingDownMessage).WithTransport(kJsonRpcTransportName);
  }
  if (resolved_interface_.transport != PreferredTransport::kJsonRpc) {
    return core::Error::Validation(std::string(kJsonRpcInterfaceRequiredMessage));
  }
  if (resolved_interface_.url.empty()) {
    return core::Error::Validation(std::string(kJsonRpcUrlRequiredMessage));
  }
  if (stream_requester_ == nullptr && cancellable_stream_requester_ == nullptr &&
      default_async_stream_client_ == nullptr) {
    return core::Error::Internal(kJsonRpcStreamRequesterNotConfiguredMessage);
  }
  const std::string request_id = id_generator_();
  if (request_id.empty()) {
    return core::Error::Internal(std::string(kEmptyRequestIdMessage));
  }
  const auto envelope_json = BuildJsonRpcEnvelope(method_name, request, request_id);
  if (!envelope_json.ok()) {
    return envelope_json.error();
  }
  HttpRequest http_request;
  http_request.method = std::string(core::http::kMethodPost);
  http_request.url = JoinUrl(resolved_interface_.url);
  http_request.body = envelope_json.value();
  http_request.timeout = options.timeout.value_or(default_timeout_);
  http_request.headers = options.headers;
  http_request.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  http_request.headers[std::string(core::http::kContentTypeHeaderName)] =
      std::string(core::http::kContentTypeApplicationJson);
  http_request.headers[std::string(core::http::kAcceptHeaderName)] =
      std::string(core::http::kContentTypeTextEventStream);
  http_request.mtls = options.mtls;
  if (!options.extensions.empty()) {
    http_request.headers[std::string(core::Extensions::kHeaderName)] = core::Extensions::Format(options.extensions);
  }
  if (options.auth_hook) {
    options.auth_hook(http_request.headers);
  }
  if (options.credential_provider != nullptr) {
    const auto applied =
        ApplyCredentialProvider(*options.credential_provider, options.auth_context, &http_request.headers);
    if (!applied.ok()) {
      return applied.error();
    }
  }
  auto state = std::make_shared<StreamHandle::State>();
  std::shared_ptr<a2a::http::Client> async_client;
  {
    std::lock_guard lock(async_client_mutex_);
    if (async_shutdown_->load()) {
      return core::Error::Network(kJsonRpcTransportShuttingDownMessage).WithTransport(kJsonRpcTransportName);
    }
    async_client = default_async_stream_client_;
  }
  if (async_client != nullptr) {
    const auto shutdown = async_shutdown_;
    auto shared_request = ToSharedHttpRequest(http_request);
    auto session = std::make_shared<JsonRpcSseSession>(HttpStreamRequesterWithCancellation{}, std::move(http_request),
                                                       observer, state, request_id);
    const auto started = async_client->StartStreamRequest(
        std::move(shared_request),
        [session, state, shutdown](const a2a::http::Response& metadata) {
          return HandleAsyncJsonRpcMetadata(session, state, shutdown, metadata);
        },
        [session, state, shutdown](std::string_view chunk) {
          return HandleAsyncJsonRpcChunk(session, state, shutdown, chunk);
        },
        [state] { return state->cancel_requested.load(); }, MakeStreamCancellationRegistrar(state),
        [session, state, shutdown](core::Result<a2a::http::Response> response) {
          CompleteAsyncJsonRpcStream(session, state, shutdown, std::move(response));
        });
    if (!started.ok()) {
      return started.error();
    }
    return std::unique_ptr<StreamHandle>(new StreamHandle(state));
  }
  StreamHandle::WorkerThread worker([stream_requester = stream_requester_,
                                     cancellable_stream_requester = cancellable_stream_requester_,
                                     http_request = std::move(http_request), &observer, state, request_id]() mutable {
    if (cancellable_stream_requester) {
      RunJsonRpcSseWorker(cancellable_stream_requester, std::move(http_request), observer, state, request_id);
    } else {
      RunJsonRpcSseWorker(stream_requester, std::move(http_request), observer, state, request_id);
    }
  });
  return std::unique_ptr<StreamHandle>(new StreamHandle(state, std::move(worker)));
}

core::Result<std::unique_ptr<StreamHandle>> JsonRpcTransport::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) {
  return StartSseStream(core::json_rpc::MethodNames::kSendStreamingMessage, request, observer, options);
}

core::Result<std::unique_ptr<StreamHandle>> JsonRpcTransport::SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                            StreamObserver& observer,
                                                                            const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  return StartSseStream(core::json_rpc::MethodNames::kSubscribeToTask, request, observer, options);
}

core::Result<void> JsonRpcTransport::Shutdown() {
  std::shared_ptr<a2a::http::Client> async_client;
  {
    std::lock_guard lock(async_client_mutex_);
    async_shutdown_->store(true);
    async_client = default_async_stream_client_;
  }
  if (async_client != nullptr) {
    async_client->Shutdown();
  }
  return {};
}


}  // namespace a2a::client
