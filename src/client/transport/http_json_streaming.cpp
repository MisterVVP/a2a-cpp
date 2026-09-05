// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "a2a/client/http_json_transport.h"
#include "a2a/client/sse_parser.h"
#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/protojson.h"
#include "http_json_transport_internal.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "a2a/core/task_states.h"
#include "core/subscription_diagnostics.h"
#endif
#include "a2a/core/version.h"
#include "a2a/http/http_client.h"

namespace a2a::client {
namespace {

using http_json_internal::BuildHttpError;
using http_json_internal::BuildTaskPath;
using http_json_internal::EndpointMap;
using http_json_internal::JoinUrl;
using http_json_internal::ToClientHttpResponse;
using http_json_internal::ToSharedHttpRequest;
using http_json_internal::ValidateResponseVersion;

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr std::string_view kDefaultMtlsUnsupportedMessage =
    "default libcurl HTTP requester does not support mTLS options; inject a custom requester for mTLS";
constexpr char kHttpTransportName[] = "http";
constexpr char kHttpTransportShuttingDownMessage[] = "HTTP transport is shutting down";
constexpr char kHttpStreamRequesterNotConfiguredMessage[] = "HTTP stream requester is not configured";
constexpr char kHttpStreamContentTypeMessage[] = "HTTP stream response must use text/event-stream";
constexpr char kHttpStreamMetadataOrderMessage[] = "HTTP stream metadata must be validated before body chunks";
constexpr std::string_view kRemoteStreamErrorMessage = "Remote stream reported error event";
constexpr std::string_view kErrorEventName = "error";
constexpr std::string_view kCodeMemberName = "code";
constexpr std::string_view kMessageMemberName = "message";
constexpr std::string_view kRestInterfaceRequiredMessage = "HttpJsonTransport requires a REST interface";
constexpr std::string_view kRestUrlRequiredMessage = "Resolved REST interface URL is required";
constexpr std::string_view kTaskIdRequiredMessage = "GetTaskRequest.id is required";
constexpr std::string_view kSubscribeSuffix = ":subscribe";
constexpr std::string_view kHistoryLengthQueryPrefix = "?historyLength=";

bool HasSseContentType(const HeaderMap& headers) {
  const auto content_type = core::http::FindHeaderValue(headers, core::http::kContentTypeHeaderName);
  return content_type.has_value() && core::http::IsSseContentType(content_type.value());
}

core::Error BuildRemoteStreamEventError(std::string_view payload_json) {
  google::protobuf::Struct payload;
  const auto parse = core::JsonToMessage(std::string(payload_json), &payload, {.ignore_unknown_fields = true});

  core::Error error =
      core::Error::RemoteProtocol(std::string(kRemoteStreamErrorMessage)).WithTransport(kHttpTransportName);
  if (!parse.ok()) {
    return error;
  }

  const auto code = payload.fields().find(kCodeMemberName);
  if (code != payload.fields().end() && code->second.kind_case() == ::google::protobuf::Value::kStringValue) {
    error = error.WithProtocolCode(code->second.string_value());
  }

  const auto message = payload.fields().find(kMessageMemberName);
  if (message != payload.fields().end() && message->second.kind_case() == ::google::protobuf::Value::kStringValue) {
    error = core::Error::RemoteProtocol(message->second.string_value())
                .WithTransport(kHttpTransportName)
                .WithProtocolCode(error.protocol_code().value_or(""));
  }
  return error;
}

core::Result<void> DispatchSseEvent(const SseEvent& event, StreamObserver& observer) {
  if (event.event == kErrorEventName) {
    auto error = BuildRemoteStreamEventError(event.data);
    return error;
  }

  lf::a2a::v1::StreamResponse response;
  const auto parsed = core::JsonToMessage(event.data, &response);
  if (!parsed.ok()) {
    auto error = parsed.error().WithTransport(kHttpTransportName);
    return error;
  }

#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  const bool terminal =
      response.has_status_update() && core::IsTerminalTaskState(response.status_update().status().state());
  {
    const core::subscription_diagnostics::ScopedTimer observation_timer(
        core::subscription_diagnostics::Phase::kClientTerminalObserverCallback, terminal);
    observer.OnEvent(response);
  }
#else
  observer.OnEvent(response);
#endif
  return {};
}

void MarkInactive(StreamHandle::State& state) { state.active.store(false); }

StreamCancellationRegistrar MakeStreamCancellationRegistrar(const std::shared_ptr<StreamHandle::State>& state) {
  return [weak_state = std::weak_ptr<StreamHandle::State>(state)](const std::function<void()>& callback) {
    if (const auto locked_state = weak_state.lock()) {
      locked_state->RegisterCancelCallback(callback);
    }
  };
}

void NotifyErrorAndStop(StreamHandle::State& state, StreamObserver& observer, const core::Error& error) {
  observer.OnError(error);
  MarkInactive(state);
}

struct HttpSseSession final {
  HttpStreamRequester requester;
  HttpStreamRequesterWithCancellation cancellable_requester;
  HttpRequest request;
  std::shared_ptr<StreamHandle::State> state;
  StreamObserver* observer = nullptr;
  std::string method;
  std::string endpoint;
  SseParser parser;
  HttpClientResponse response_metadata;
  bool metadata_validated = false;
  bool collecting_error_body = false;

  core::Result<void> ValidateMetadata(const HttpClientResponse& response) {
    response_metadata = response;
    const auto version_check = ValidateResponseVersion(response_metadata);
    if (!version_check.ok()) {
      return version_check.error();
    }
    if (response_metadata.status_code < kHttpOkMin || response_metadata.status_code > kHttpOkMax) {
      collecting_error_body = true;
      metadata_validated = true;
      return {};
    }
    if (!HasSseContentType(response_metadata.headers)) {
      return core::Error::RemoteProtocol(kHttpStreamContentTypeMessage)
          .WithTransport(kHttpTransportName)
          .WithHttpStatus(response_metadata.status_code);
    }
    metadata_validated = true;
    return {};
  }

  core::Result<void> HandleChunk(std::string_view chunk) {
    if (state->cancel_requested.load()) {
      return {};
    }
    if (!metadata_validated) {
      return core::Error::RemoteProtocol(kHttpStreamMetadataOrderMessage).WithTransport(kHttpTransportName);
    }
    if (collecting_error_body) {
      response_metadata.body.append(chunk);
      return {};
    }
    return parser.Feed(chunk, [this](const SseEvent& event) -> core::Result<void> {
      if (state->cancel_requested.load()) {
        return {};
      }
      return DispatchSseEvent(event, *observer);
    });
  }

  void Run() {
    const auto metadata_handler = [this](const HttpClientResponse& metadata) { return ValidateMetadata(metadata); };
    const auto chunk_handler = [this](std::string_view chunk) { return HandleChunk(chunk); };
    const auto cancelled = [this] { return state->cancel_requested.load(); };
    const auto response = cancellable_requester
                              ? cancellable_requester(request, metadata_handler, chunk_handler, cancelled,
                                                      MakeStreamCancellationRegistrar(state))
                              : requester(request, metadata_handler, chunk_handler, cancelled);
    Complete(response);
  }

  void Complete(const core::Result<HttpClientResponse>& response) {
    if (state->cancel_requested.load()) {
      MarkInactive(*state);
      return;
    }
    if (!response.ok()) {
      NotifyErrorAndStop(*state, *observer, response.error());
      return;
    }
    if (!metadata_validated) {
      const auto metadata = ValidateMetadata(response.value());
      if (!metadata.ok()) {
        NotifyErrorAndStop(*state, *observer, metadata.error());
        return;
      }
    }
    if (collecting_error_body) {
      if (response_metadata.body.empty()) {
        response_metadata.body = response.value().body;
      }
      NotifyErrorAndStop(*state, *observer, BuildHttpError(method, endpoint, response_metadata));
      return;
    }
    const auto finish = parser.Finish([this](const SseEvent& event) -> core::Result<void> {
      if (state->cancel_requested.load()) {
        return {};
      }
      return DispatchSseEvent(event, *observer);
    });
    if (state->cancel_requested.load()) {
      MarkInactive(*state);
      return;
    }
    if (!finish.ok()) {
      NotifyErrorAndStop(*state, *observer, finish.error());
      return;
    }
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    {
      const core::subscription_diagnostics::ScopedTimer completion_timer(
          core::subscription_diagnostics::Phase::kClientCompletionCallback);
      observer->OnCompleted();
    }
#else
    observer->OnCompleted();
#endif
    MarkInactive(*state);
  }
};

core::Result<HttpRequest> BuildStreamingRequest(const ResolvedInterface& resolved_interface, HttpOperation operation,
                                                std::string body, const CallOptions& options,
                                                std::chrono::milliseconds default_timeout) {
  if (resolved_interface.transport != PreferredTransport::kRest) {
    return core::Error::Validation(std::string(kRestInterfaceRequiredMessage));
  }
  if (resolved_interface.url.empty()) {
    return core::Error::Validation(std::string(kRestUrlRequiredMessage));
  }

  HttpRequest request;
  request.method = std::string(operation.method);
  request.url = JoinUrl(resolved_interface.url, operation.endpoint);
  request.body = std::move(body);
  request.timeout = options.timeout.value_or(default_timeout);
  request.headers = options.headers;
  request.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  if (!request.body.empty()) {
    request.headers[std::string(core::http::kContentTypeHeaderName)] =
        std::string(core::http::kContentTypeApplicationJson);
  }
  request.headers[std::string(core::http::kAcceptHeaderName)] = std::string(core::http::kContentTypeTextEventStream);
  request.mtls = options.mtls;

  if (!options.extensions.empty()) {
    request.headers[std::string(core::Extensions::kHeaderName)] = core::Extensions::Format(options.extensions);
  }
  if (options.auth_hook) {
    options.auth_hook(request.headers);
  }
  if (options.credential_provider != nullptr) {
    const auto applied = ApplyCredentialProvider(*options.credential_provider, options.auth_context, &request.headers);
    if (!applied.ok()) {
      return applied.error();
    }
  }
  return request;
}

}  // namespace

HttpStreamRequester MakeDefaultHttpStreamRequester() {
  return [client = a2a::http::Client{}](const HttpRequest& request, const HttpStreamMetadataHandler& on_metadata,
                                        const HttpStreamChunkHandler& on_chunk,
                                        const StreamCancelled& is_cancelled) -> core::Result<HttpClientResponse> {
    if (request.mtls.has_value()) {
      return core::Error::Validation(std::string(kDefaultMtlsUnsupportedMessage));
    }
    auto response = client.StreamRequest(
        ToSharedHttpRequest(request),
        [&on_metadata](const a2a::http::Response& metadata) {
          return on_metadata(ToClientHttpResponse(a2a::http::Response{
              .status_code = metadata.status_code, .headers = metadata.headers, .body = metadata.body}));
        },
        on_chunk, is_cancelled);
    if (!response.ok()) {
      return response.error();
    }
    return ToClientHttpResponse(std::move(response.value()));
  };
}

core::Result<std::unique_ptr<StreamHandle>> HttpJsonTransport::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) {
  const auto body = core::MessageToJson(request);
  if (!body.ok()) {
    return body.error();
  }

  return StartSseStream(
      {.method = std::string(core::http::kMethodPost), .endpoint = EndpointMap::kSendStreamingMessage}, body.value(),
      observer, options);
}

core::Result<std::unique_ptr<StreamHandle>> HttpJsonTransport::SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                             StreamObserver& observer,
                                                                             const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }

  std::string endpoint = BuildTaskPath(request.id());
  endpoint.append(kSubscribeSuffix);
  if (request.has_history_length()) {
    endpoint.append(kHistoryLengthQueryPrefix);
    endpoint.append(std::to_string(request.history_length()));
  }

  return StartSseStream({.method = std::string(core::http::kMethodGet), .endpoint = endpoint}, {}, observer, options);
}

core::Result<void> HttpJsonTransport::Shutdown() {
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

core::Result<std::unique_ptr<StreamHandle>> HttpJsonTransport::StartSseStream(HttpOperation operation, std::string body,
                                                                              StreamObserver& observer,
                                                                              const CallOptions& options) const {
  if (async_shutdown_->load()) {
    return core::Error::Network(kHttpTransportShuttingDownMessage).WithTransport(kHttpTransportName);
  }
  if (stream_requester_ == nullptr && cancellable_stream_requester_ == nullptr &&
      default_async_stream_client_ == nullptr) {
    return core::Error::Internal(kHttpStreamRequesterNotConfiguredMessage);
  }
  auto request = BuildStreamingRequest(resolved_interface_, operation, std::move(body), options, default_timeout_);
  if (!request.ok()) {
    return request.error();
  }

  auto state = std::make_shared<StreamHandle::State>();
  auto session = std::make_shared<HttpSseSession>(HttpSseSession{.requester = stream_requester_,
                                                                 .cancellable_requester = cancellable_stream_requester_,
                                                                 .request = std::move(request.value()),
                                                                 .state = state,
                                                                 .observer = &observer,
                                                                 .method = std::string(operation.method),
                                                                 .endpoint = std::string(operation.endpoint),
                                                                 .parser = {},
                                                                 .response_metadata = {},
                                                                 .metadata_validated = false,
                                                                 .collecting_error_body = false});
  std::shared_ptr<a2a::http::Client> async_client;
  {
    std::lock_guard lock(async_client_mutex_);
    if (async_shutdown_->load()) {
      return core::Error::Network(kHttpTransportShuttingDownMessage).WithTransport(kHttpTransportName);
    }
    async_client = default_async_stream_client_;
  }
  if (async_client != nullptr) {
    const auto shutdown = async_shutdown_;
    const auto started = async_client->StartStreamRequest(
        ToSharedHttpRequest(session->request),
        [session, state, shutdown](const a2a::http::Response& metadata) {
          StreamHandle::State::CallbackExecutionScope callback_scope(*state);
          if (shutdown->load()) {
            return core::Result<void>{
                core::Error::Network(kHttpTransportShuttingDownMessage).WithTransport(kHttpTransportName)};
          }
          return session->ValidateMetadata(ToClientHttpResponse(a2a::http::Response{
              .status_code = metadata.status_code, .headers = metadata.headers, .body = metadata.body}));
        },
        [session, state, shutdown](std::string_view chunk) {
          StreamHandle::State::CallbackExecutionScope callback_scope(*state);
          if (shutdown->load()) {
            return core::Result<void>{
                core::Error::Network(kHttpTransportShuttingDownMessage).WithTransport(kHttpTransportName)};
          }
          return session->HandleChunk(chunk);
        },
        [state] { return state->cancel_requested.load(); }, MakeStreamCancellationRegistrar(state),
        [session, state, shutdown](core::Result<a2a::http::Response> response) {
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
        });
    if (!started.ok()) {
      return started.error();
    }
    return std::unique_ptr<StreamHandle>(new StreamHandle(state));
  }
  auto worker = StreamHandle::WorkerThread([session = std::move(session)] { session->Run(); });
  return std::unique_ptr<StreamHandle>(new StreamHandle(state, std::move(worker)));
}

}  // namespace a2a::client
