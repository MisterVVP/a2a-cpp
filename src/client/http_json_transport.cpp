// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/http_json_transport.h"

#include <array>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "a2a/client/sse_parser.h"
#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/protocol_methods.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "a2a/http/http_client.h"

namespace a2a::client {
namespace {

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr int kHttpNoContent = 204;
constexpr std::string_view kDefaultMtlsUnsupportedMessage =
    "default libcurl HTTP requester does not support mTLS options; inject a custom requester for mTLS";

a2a::http::Request ToSharedHttpRequest(const HttpRequest& request) {
  a2a::http::Request shared_request;
  shared_request.method = request.method;
  shared_request.url = request.url;
  shared_request.body = request.body;
  shared_request.timeout = request.timeout;
  shared_request.headers.reserve(request.headers.size());
  for (const auto& [name, value] : request.headers) {
    shared_request.headers.push_back(a2a::http::Header{.name = name, .value = value});
  }
  return shared_request;
}

HttpClientResponse ToClientHttpResponse(a2a::http::Response response) {
  HttpClientResponse client_response;
  client_response.status_code = response.status_code;
  client_response.body = std::move(response.body);
  client_response.headers.reserve(response.headers.size());
  for (auto& header : response.headers) {
    client_response.headers.insert_or_assign(std::move(header.name), std::move(header.value));
  }
  return client_response;
}

struct EndpointMap final {
  static constexpr std::string_view kSendMessage = "/message:send";
  static constexpr std::string_view kSendStreamingMessage = "/message:stream";
  static constexpr std::string_view kTaskCollection = "/tasks";
  static constexpr std::string_view kPushConfigCollection = core::protocol_methods::kPushNotificationConfigsSegment;
};

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::string JoinUrl(std::string_view interface_base_url, std::string_view rpc_endpoint) {
  std::string base(interface_base_url);
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  if (rpc_endpoint.empty()) {
    return base;
  }
  if (!rpc_endpoint.starts_with('/')) {
    return base + "/" + std::string(rpc_endpoint);
  }
  return base + std::string(rpc_endpoint);
}

bool HasSseContentType(const HeaderMap& headers) {
  const auto content_type = core::http::FindHeaderValue(headers, core::http::kContentTypeHeaderName);
  return content_type.has_value() && core::http::IsSseContentType(content_type.value());
}

core::Result<void> ValidateResponseVersion(const HttpClientResponse& response) {
  const auto version = core::http::FindHeaderValue(response.headers, core::Version::kHeaderName);
  if (!version.has_value()) {
    return {};
  }
  if (!core::Version::IsSupported(version.value())) {
    return core::Error::UnsupportedVersion("Server returned unsupported A2A-Version header")
        .WithTransport("http")
        .WithProtocolCode(std::string(version.value()));
  }
  return {};
}

core::Error BuildHttpError(std::string_view method, std::string_view endpoint, const HttpClientResponse& response) {
  std::ostringstream stream;
  stream << "HTTP request failed for " << method << " " << endpoint;
  if (!response.body.empty()) {
    stream << ": " << response.body;
  }

  core::Error error = core::Error::RemoteProtocol(stream.str()).WithTransport("http");
  error = error.WithHttpStatus(response.status_code);

  if (!response.body.empty() && response.body.front() == '{') {
    google::protobuf::Struct status_payload;
    if (core::JsonToMessage(response.body, &status_payload, {.ignore_unknown_fields = true}).ok()) {
      const auto code = status_payload.fields().find("code");
      if (code != status_payload.fields().end() &&
          code->second.kind_case() == ::google::protobuf::Value::kStringValue) {
        error = error.WithProtocolCode(code->second.string_value());
      }
    }
  }
  return error;
}

template <typename T>
core::Result<T> ParseBodyOrMapError(std::string_view method, std::string_view endpoint,
                                    const HttpClientResponse& response) {
  if (response.status_code < kHttpOkMin || response.status_code > kHttpOkMax) {
    return BuildHttpError(method, endpoint, response);
  }

  T parsed;
  const auto parse = core::JsonToMessage(response.body, &parsed);
  if (!parse.ok()) {
    return parse.error().WithTransport("http").WithHttpStatus(response.status_code);
  }
  return parsed;
}

std::string BuildTaskPath(std::string_view task_id) {
  std::string path;
  path.reserve(EndpointMap::kTaskCollection.size() + 1 + task_id.size());
  path += EndpointMap::kTaskCollection;
  path += '/';
  path += task_id;
  return path;
}

std::string BuildTaskPushConfigCollectionPath(std::string_view task_id) {
  std::string path = BuildTaskPath(task_id);
  path.reserve(path.size() + EndpointMap::kPushConfigCollection.size());
  path += EndpointMap::kPushConfigCollection;
  return path;
}

struct PushConfigPathParts final {
  std::string_view task_id;
  std::string_view id;
};

std::string BuildTaskPushConfigPath(PushConfigPathParts parts) {
  std::string path = BuildTaskPushConfigCollectionPath(parts.task_id);
  path.reserve(path.size() + 1 + parts.id.size());
  path += '/';
  path += parts.id;
  return path;
}

core::Error BuildRemoteStreamEventError(std::string_view payload_json) {
  google::protobuf::Struct payload;
  const auto parse = core::JsonToMessage(std::string(payload_json), &payload, {.ignore_unknown_fields = true});

  core::Error error = core::Error::RemoteProtocol("Remote stream reported error event").WithTransport("http");
  if (!parse.ok()) {
    return error;
  }

  const auto code = payload.fields().find("code");
  if (code != payload.fields().end() && code->second.kind_case() == ::google::protobuf::Value::kStringValue) {
    error = error.WithProtocolCode(code->second.string_value());
  }

  const auto message = payload.fields().find("message");
  if (message != payload.fields().end() && message->second.kind_case() == ::google::protobuf::Value::kStringValue) {
    error = core::Error::RemoteProtocol(message->second.string_value())
                .WithTransport("http")
                .WithProtocolCode(error.protocol_code().value_or(""));
  }
  return error;
}

core::Result<void> DispatchSseEvent(const SseEvent& event, StreamObserver& observer) {
  if (event.event == "error") {
    auto error = BuildRemoteStreamEventError(event.data);
    return error;
  }

  lf::a2a::v1::StreamResponse response;
  const auto parsed = core::JsonToMessage(event.data, &response);
  if (!parsed.ok()) {
    auto error = parsed.error().WithTransport("http");
    return error;
  }

  observer.OnEvent(response);
  return {};
}

core::Result<ListTasksResponse> ParseListTasksResponsePayload(const HttpClientResponse& response,
                                                              std::string_view endpoint) {
  if (response.status_code < kHttpOkMin || response.status_code > kHttpOkMax) {
    return BuildHttpError(core::http::kMethodGet, endpoint, response);
  }

  google::protobuf::Struct payload;
  const auto parse = core::JsonToMessage(response.body, &payload, {.ignore_unknown_fields = true});
  if (!parse.ok()) {
    return parse.error().WithTransport("http").WithHttpStatus(response.status_code);
  }

  ListTasksResponse parsed;
  const auto tasks_it = payload.fields().find("tasks");
  if (tasks_it != payload.fields().end()) {
    if (!tasks_it->second.has_list_value()) {
      return core::Error::Serialization("ListTasks response field 'tasks' must be an array")
          .WithTransport("http")
          .WithHttpStatus(response.status_code);
    }
    for (const auto& task_value : tasks_it->second.list_value().values()) {
      const auto task_json = core::MessageToJson(task_value);
      if (!task_json.ok()) {
        return task_json.error().WithTransport("http").WithHttpStatus(response.status_code);
      }
      lf::a2a::v1::Task task;
      const auto task_parse = core::JsonToMessage(task_json.value(), &task, {.ignore_unknown_fields = true});
      if (!task_parse.ok()) {
        return task_parse.error().WithTransport("http").WithHttpStatus(response.status_code);
      }
      parsed.tasks.push_back(std::move(task));
    }
  }

  const auto next_token_it = payload.fields().find("nextPageToken");
  if (next_token_it != payload.fields().end()) {
    if (next_token_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
      return core::Error::Serialization("ListTasks response field 'nextPageToken' must be a string")
          .WithTransport("http")
          .WithHttpStatus(response.status_code);
    }
    parsed.next_page_token = next_token_it->second.string_value();
  }

  return parsed;
}

void MarkInactive(StreamHandle::State& state) { state.active.store(false); }

void NotifyErrorAndStop(StreamHandle::State& state, StreamObserver& observer, const core::Error& error) {
  observer.OnError(error);
  MarkInactive(state);
}

core::Result<HttpRequest> BuildStreamingRequest(const ResolvedInterface& resolved_interface, HttpOperation operation,
                                                std::string body, const CallOptions& options,
                                                std::chrono::milliseconds default_timeout) {
  if (resolved_interface.transport != PreferredTransport::kRest) {
    return core::Error::Validation("HttpJsonTransport requires a REST interface");
  }
  if (resolved_interface.url.empty()) {
    return core::Error::Validation("Resolved REST interface URL is required");
  }

  HttpRequest request;
  request.method = std::string(operation.method);
  request.url = JoinUrl(resolved_interface.url, operation.endpoint);
  request.body = std::move(body);
  request.timeout = options.timeout.value_or(default_timeout);
  request.headers = options.headers;
  request.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  request.headers["Content-Type"] = "application/json";
  request.headers["Accept"] = "text/event-stream";
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

HttpRequester MakeDefaultHttpRequester() {
  return [client = a2a::http::Client{}](const HttpRequest& request) -> core::Result<HttpClientResponse> {
    if (request.mtls.has_value()) {
      return core::Error::Validation(std::string(kDefaultMtlsUnsupportedMessage));
    }
    auto response = client.SendRequest(ToSharedHttpRequest(request));
    if (!response.ok()) {
      return response.error();
    }
    return ToClientHttpResponse(std::move(response.value()));
  };
}

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

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     HttpStreamRequester stream_requester, std::chrono::milliseconds default_timeout)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      stream_requester_(std::move(stream_requester)),
      default_timeout_(default_timeout) {}

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     std::chrono::milliseconds default_timeout)
    : HttpJsonTransport(std::move(resolved_interface), std::move(requester), {}, default_timeout) {}

std::unique_ptr<HttpJsonTransport> HttpJsonTransport::CreateDefault(ResolvedInterface resolved_interface,
                                                                    std::chrono::milliseconds default_timeout) {
  return std::make_unique<HttpJsonTransport>(std::move(resolved_interface), MakeDefaultHttpRequester(),
                                             MakeDefaultHttpStreamRequester(), default_timeout);
}

core::Result<HttpClientResponse> HttpJsonTransport::SendRequest(HttpOperation operation, std::string body,
                                                                const CallOptions& options) const {
  if (resolved_interface_.transport != PreferredTransport::kRest) {
    return core::Error::Validation("HttpJsonTransport requires a REST interface");
  }
  if (requester_ == nullptr) {
    return core::Error::Internal("HTTP requester is not configured");
  }
  if (resolved_interface_.url.empty()) {
    return core::Error::Validation("Resolved REST interface URL is required");
  }

  HttpRequest request;
  request.method = std::string(operation.method);
  request.url = JoinUrl(resolved_interface_.url, operation.endpoint);
  request.body = std::move(body);
  request.timeout = options.timeout.value_or(default_timeout_);

  request.headers = options.headers;
  request.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  request.headers["Content-Type"] = "application/json";
  request.headers["Accept"] = "application/json";
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

  const auto response = requester_(request);
  if (!response.ok()) {
    return response.error();
  }

  const auto version_check = ValidateResponseVersion(response.value());
  if (!version_check.ok()) {
    return version_check.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::SendMessageResponse> HttpJsonTransport::SendMessage(
    const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) {
  const auto body = core::MessageToJson(request);
  if (!body.ok()) {
    return body.error();
  }

  const std::string endpoint(EndpointMap::kSendMessage);
  const auto response =
      SendRequest({.method = std::string(core::http::kMethodPost), .endpoint = endpoint}, body.value(), options);
  if (!response.ok()) {
    return response.error();
  }

  return ParseBodyOrMapError<lf::a2a::v1::SendMessageResponse>(core::http::kMethodPost, endpoint, response.value());
}

core::Result<lf::a2a::v1::Task> HttpJsonTransport::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                           const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskRequest.id is required");
  }

  std::string endpoint = BuildTaskPath(request.id());
  if (request.has_history_length()) {
    endpoint += "?historyLength=" + std::to_string(request.history_length());
  }

  const auto response = SendRequest({.method = std::string(core::http::kMethodGet), .endpoint = endpoint}, {}, options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::Task>(core::http::kMethodGet, endpoint, response.value());
}

core::Result<ListTasksResponse> HttpJsonTransport::ListTasks(const ListTasksRequest& request,
                                                             const CallOptions& options) {
  std::ostringstream endpoint;
  endpoint << EndpointMap::kTaskCollection;
  if (request.page_size > 0 || !request.page_token.empty()) {
    endpoint << "?";
    bool has_previous = false;
    if (request.page_size > 0) {
      endpoint << "pageSize=" << request.page_size;
      has_previous = true;
    }
    if (!request.page_token.empty()) {
      if (has_previous) {
        endpoint << "&";
      }
      endpoint << "pageToken=" << request.page_token;
    }
  }

  const std::string endpoint_path = endpoint.str();
  const auto response =
      SendRequest({.method = std::string(core::http::kMethodGet), .endpoint = endpoint_path}, {}, options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseListTasksResponsePayload(response.value(), endpoint_path);
}

core::Result<lf::a2a::v1::Task> HttpJsonTransport::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                              const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("CancelTaskRequest.id is required");
  }

  const std::string endpoint = BuildTaskPath(request.id()) + ":cancel";
  const auto response =
      SendRequest({.method = std::string(core::http::kMethodPost), .endpoint = endpoint}, "{}", options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::Task>(core::http::kMethodPost, endpoint, response.value());
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> HttpJsonTransport::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  if (request.task_id().empty()) {
    return core::Error::Validation("TaskPushNotificationConfig.task_id is required");
  }

  const auto body = core::MessageToJson(request);
  if (!body.ok()) {
    return body.error();
  }

  const std::string endpoint = BuildTaskPushConfigCollectionPath(request.task_id());
  const auto response =
      SendRequest({.method = std::string(core::http::kMethodPost), .endpoint = endpoint}, body.value(), options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::TaskPushNotificationConfig>(core::http::kMethodPost, endpoint,
                                                                      response.value());
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> HttpJsonTransport::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (request.task_id().empty()) {
    return core::Error::Validation("GetTaskPushNotificationConfigRequest.task_id is required");
  }
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskPushNotificationConfigRequest.id is required");
  }

  const std::string endpoint = BuildTaskPushConfigPath({.task_id = request.task_id(), .id = request.id()});
  const auto response = SendRequest({.method = std::string(core::http::kMethodGet), .endpoint = endpoint}, {}, options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::TaskPushNotificationConfig>(core::http::kMethodGet, endpoint,
                                                                      response.value());
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> HttpJsonTransport::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) {
  if (request.task_id().empty()) {
    return core::Error::Validation("ListTaskPushNotificationConfigsRequest.task_id is required");
  }

  std::ostringstream endpoint;
  endpoint << BuildTaskPushConfigCollectionPath(request.task_id());
  if (request.page_size() > 0 || !request.page_token().empty()) {
    endpoint << "?";
    bool has_previous = false;
    if (request.page_size() > 0) {
      endpoint << "pageSize=" << request.page_size();
      has_previous = true;
    }
    if (!request.page_token().empty()) {
      if (has_previous) {
        endpoint << "&";
      }
      endpoint << "pageToken=" << request.page_token();
    }
  }

  const std::string path = endpoint.str();
  const auto response = SendRequest({.method = std::string(core::http::kMethodGet), .endpoint = path}, {}, options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(core::http::kMethodGet, path,
                                                                                   response.value());
}

core::Result<void> HttpJsonTransport::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (request.task_id().empty()) {
    return core::Error::Validation("DeleteTaskPushNotificationConfigRequest.task_id is required");
  }
  if (request.id().empty()) {
    return core::Error::Validation("DeleteTaskPushNotificationConfigRequest.id is required");
  }

  const std::string endpoint = BuildTaskPushConfigPath({.task_id = request.task_id(), .id = request.id()});
  const auto response =
      SendRequest({.method = std::string(core::http::kMethodDelete), .endpoint = endpoint}, {}, options);
  if (!response.ok()) {
    return response.error();
  }

  if (response.value().status_code < kHttpOkMin || response.value().status_code > kHttpOkMax) {
    return BuildHttpError(core::http::kMethodDelete, endpoint, response.value());
  }

  if (response.value().status_code != kHttpNoContent && !response.value().body.empty() &&
      response.value().body != "{}") {
    google::protobuf::Empty ignored;
    const auto parse = core::JsonToMessage(response.value().body, &ignored);
    if (!parse.ok()) {
      return parse.error().WithTransport("http").WithHttpStatus(response.value().status_code);
    }
  }

  return {};
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
    return core::Error::Validation("GetTaskRequest.id is required");
  }

  std::string endpoint = BuildTaskPath(request.id()) + ":subscribe";
  if (request.has_history_length()) {
    endpoint += "?historyLength=" + std::to_string(request.history_length());
  }

  return StartSseStream({.method = std::string(core::http::kMethodGet), .endpoint = endpoint}, {}, observer, options);
}

core::Result<std::unique_ptr<StreamHandle>> HttpJsonTransport::StartSseStream(HttpOperation operation, std::string body,
                                                                              StreamObserver& observer,
                                                                              const CallOptions& options) const {
  if (stream_requester_ == nullptr) {
    return core::Error::Internal("HTTP stream requester is not configured");
  }

  auto request = BuildStreamingRequest(resolved_interface_, operation, std::move(body), options, default_timeout_);
  if (!request.ok()) {
    return request.error();
  }

  auto state = std::make_shared<StreamHandle::State>();
  auto worker = StreamHandle::WorkerThread([this, request = std::move(request.value()), state, &observer,
                                            method = std::string(operation.method),
                                            endpoint = std::string(operation.endpoint)]() mutable {
    SseParser parser;

    HttpClientResponse response_metadata;
    bool metadata_validated = false;
    const auto validate_metadata = [&response_metadata, &metadata_validated, &method,
                                    &endpoint](const HttpClientResponse& response) -> core::Result<void> {
      response_metadata = response;
      const auto version_check = ValidateResponseVersion(response_metadata);
      if (!version_check.ok()) {
        return version_check.error();
      }
      if (response_metadata.status_code < kHttpOkMin || response_metadata.status_code > kHttpOkMax) {
        return BuildHttpError(method, endpoint, response_metadata);
      }
      if (!HasSseContentType(response_metadata.headers)) {
        return core::Error::RemoteProtocol("HTTP stream response must use text/event-stream")
            .WithTransport("http")
            .WithHttpStatus(response_metadata.status_code);
      }
      metadata_validated = true;
      return {};
    };

    const auto stream_response = stream_requester_(
        request, validate_metadata,
        [&parser, &observer, state, &metadata_validated](std::string_view chunk) -> core::Result<void> {
          if (state->cancel_requested.load()) {
            return {};
          }
          if (!metadata_validated) {
            return core::Error::RemoteProtocol("HTTP stream metadata must be validated before body chunks")
                .WithTransport("http");
          }
          return parser.Feed(chunk, [&observer](const SseEvent& event) { return DispatchSseEvent(event, observer); });
        },
        [state]() { return state->cancel_requested.load(); });

    if (state->cancel_requested.load()) {
      MarkInactive(*state);
      return;
    }

    if (!stream_response.ok()) {
      NotifyErrorAndStop(*state, observer, stream_response.error());
      return;
    }

    const auto finish = parser.Finish([&observer](const SseEvent& event) { return DispatchSseEvent(event, observer); });
    if (!finish.ok()) {
      NotifyErrorAndStop(*state, observer, finish.error());
      return;
    }

    observer.OnCompleted();
    MarkInactive(*state);
  });

  return std::unique_ptr<StreamHandle>(new StreamHandle(state, std::move(worker)));
}

}  // namespace a2a::client
