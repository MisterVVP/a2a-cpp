#include "a2a/client/http_json_transport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <memory>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include "a2a/client/sse_parser.h"
#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::client {
namespace {

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr int kHttpNoContent = 204;

struct EndpointMap final {
  static constexpr std::string_view kSendMessage = "/messages:send";
  static constexpr std::string_view kSendStreamingMessage = "/messages:stream";
  static constexpr std::string_view kTaskCollection = "/tasks";
  static constexpr std::string_view kPushConfigCollection = "/pushNotificationConfigs";
};

std::string ToLower(std::string_view value) {
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lowered;
}

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

std::string FindHeaderValue(const HeaderMap& headers, std::string_view name) {
  const std::string lowered_name = ToLower(name);
  for (const auto& [header_name, value] : headers) {
    if (ToLower(header_name) == lowered_name) {
      return value;
    }
  }
  return {};
}

core::Result<void> ValidateResponseVersion(const HttpClientResponse& response) {
  const std::string version = FindHeaderValue(response.headers, core::Version::kHeaderName);
  if (version.empty()) {
    return {};
  }
  if (!core::Version::IsSupported(version)) {
    return core::Error::UnsupportedVersion("Server returned unsupported A2A-Version header")
        .WithTransport("http")
        .WithProtocolCode(version);
  }
  return {};
}

core::Error BuildHttpError(std::string_view method, std::string_view endpoint,
                           const HttpClientResponse& response) {
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
      if (code != status_payload.fields().end() && code->second.has_string_value()) {
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
  return std::string(EndpointMap::kTaskCollection) + "/" + std::string(task_id);
}

std::string BuildPushConfigPath(std::string_view id) {
  return std::string(EndpointMap::kPushConfigCollection) + "/" + std::string(id);
}

core::Error BuildRemoteStreamEventError(std::string_view payload_json) {
  google::protobuf::Struct payload;
  const auto parse =
      core::JsonToMessage(std::string(payload_json), &payload, {.ignore_unknown_fields = true});

  core::Error error =
      core::Error::RemoteProtocol("Remote stream reported error event").WithTransport("http");
  if (!parse.ok()) {
    return error;
  }

  const auto code = payload.fields().find("code");
  if (code != payload.fields().end() && code->second.has_string_value()) {
    error = error.WithProtocolCode(code->second.string_value());
  }

  const auto message = payload.fields().find("message");
  if (message != payload.fields().end() && message->second.has_string_value()) {
    error = core::Error::RemoteProtocol(message->second.string_value())
                .WithTransport("http")
                .WithProtocolCode(error.protocol_code().value_or(""));
  }
  return error;
}

core::Result<void> DispatchSseEvent(const SseEvent& event, StreamObserver& observer) {
  if (event.event == "error") {
    auto error = BuildRemoteStreamEventError(event.data);
    observer.OnError(error);
    return error;
  }

  lf::a2a::v1::StreamResponse response;
  const auto parsed = core::JsonToMessage(event.data, &response);
  if (!parsed.ok()) {
    auto error = parsed.error().WithTransport("http");
    observer.OnError(error);
    return error;
  }

  observer.OnEvent(response);
  return {};
}

void MarkInactive(StreamHandle::State& state) { state.active.store(false); }

void NotifyErrorAndStop(StreamHandle::State& state, StreamObserver& observer,
                        const core::Error& error) {
  observer.OnError(error);
  MarkInactive(state);
}

core::Result<HttpRequest> BuildStreamingRequest(const ResolvedInterface& resolved_interface,
                                                HttpOperation operation, std::string body,
                                                const CallOptions& options,
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
    request.headers[std::string(core::Extensions::kHeaderName)] =
        core::Extensions::Format(options.extensions);
  }
  if (options.auth_hook) {
    options.auth_hook(request.headers);
  }
  if (options.credential_provider != nullptr) {
    const auto applied = ApplyCredentialProvider(*options.credential_provider, options.auth_context,
                                                 &request.headers);
    if (!applied.ok()) {
      return applied.error();
    }
  }
  return request;
}

}  // namespace

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     HttpStreamRequester stream_requester,
                                     std::chrono::milliseconds default_timeout)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      stream_requester_(std::move(stream_requester)),
      default_timeout_(default_timeout) {}

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     std::chrono::milliseconds default_timeout)
    : HttpJsonTransport(std::move(resolved_interface), std::move(requester), {}, default_timeout) {}

core::Result<HttpClientResponse> HttpJsonTransport::SendRequest(HttpOperation operation,
                                                                std::string body,
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
    request.headers[std::string(core::Extensions::kHeaderName)] =
        core::Extensions::Format(options.extensions);
  }

  if (options.auth_hook) {
    options.auth_hook(request.headers);
  }
  if (options.credential_provider != nullptr) {
    const auto applied = ApplyCredentialProvider(*options.credential_provider, options.auth_context,
                                                 &request.headers);
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
      SendRequest({.method = "POST", .endpoint = endpoint}, body.value(), options);
  if (!response.ok()) {
    return response.error();
  }

  return ParseBodyOrMapError<lf::a2a::v1::SendMessageResponse>("POST", endpoint, response.value());
}

core::Result<lf::a2a::v1::Task> HttpJsonTransport::GetTask(
    const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskRequest.id is required");
  }

  std::string endpoint = BuildTaskPath(request.id());
  if (!request.history_length().empty()) {
    endpoint += "?historyLength=" + request.history_length();
  }

  const auto response = SendRequest({.method = "GET", .endpoint = endpoint}, {}, options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::Task>("GET", endpoint, response.value());
}

core::Result<lf::a2a::v1::Task> HttpJsonTransport::CancelTask(
    const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("CancelTaskRequest.id is required");
  }

  const std::string endpoint = BuildTaskPath(request.id()) + ":cancel";
  const auto response = SendRequest({.method = "POST", .endpoint = endpoint}, "{}", options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::Task>("POST", endpoint, response.value());
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig>
HttpJsonTransport::SetTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  const auto body = core::MessageToJson(request);
  if (!body.ok()) {
    return body.error();
  }

  const std::string endpoint(EndpointMap::kPushConfigCollection);
  const auto response =
      SendRequest({.method = "POST", .endpoint = endpoint}, body.value(), options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::TaskPushNotificationConfig>("POST", endpoint,
                                                                      response.value());
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig>
HttpJsonTransport::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskPushNotificationConfigRequest.id is required");
  }

  const std::string endpoint = BuildPushConfigPath(request.id());
  const auto response = SendRequest({.method = "GET", .endpoint = endpoint}, {}, options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::TaskPushNotificationConfig>("GET", endpoint,
                                                                      response.value());
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
HttpJsonTransport::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
    const CallOptions& options) {
  std::ostringstream endpoint;
  endpoint << EndpointMap::kPushConfigCollection;
  if (!request.task_id().empty() || request.page_size() > 0 || !request.page_token().empty()) {
    endpoint << "?";
    bool has_previous = false;
    if (!request.task_id().empty()) {
      endpoint << "taskId=" << request.task_id();
      has_previous = true;
    }
    if (request.page_size() > 0) {
      if (has_previous) {
        endpoint << "&";
      }
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
  const auto response = SendRequest({.method = "GET", .endpoint = path}, {}, options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
      "GET", path, response.value());
}

core::Result<void> HttpJsonTransport::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
    const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("DeleteTaskPushNotificationConfigRequest.id is required");
  }

  const std::string endpoint = BuildPushConfigPath(request.id());
  const auto response = SendRequest({.method = "DELETE", .endpoint = endpoint}, {}, options);
  if (!response.ok()) {
    return response.error();
  }

  if (response.value().status_code < kHttpOkMin || response.value().status_code > kHttpOkMax) {
    return BuildHttpError("DELETE", endpoint, response.value());
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
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
    const CallOptions& options) {
  const auto body = core::MessageToJson(request);
  if (!body.ok()) {
    return body.error();
  }

  return StartSseStream({.method = "POST", .endpoint = EndpointMap::kSendStreamingMessage},
                        body.value(), observer, options);
}

core::Result<std::unique_ptr<StreamHandle>> HttpJsonTransport::SubscribeTask(
    const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer,
    const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskRequest.id is required");
  }

  std::string endpoint = BuildTaskPath(request.id()) + ":subscribe";
  if (!request.history_length().empty()) {
    endpoint += "?historyLength=" + request.history_length();
  }

  return StartSseStream({.method = "GET", .endpoint = endpoint}, {}, observer, options);
}

core::Result<std::unique_ptr<StreamHandle>> HttpJsonTransport::StartSseStream(
    HttpOperation operation, std::string body, StreamObserver& observer,
    const CallOptions& options) const {
  if (stream_requester_ == nullptr) {
    return core::Error::Internal("HTTP stream requester is not configured");
  }

  auto request = BuildStreamingRequest(resolved_interface_, operation, std::move(body), options,
                                       default_timeout_);
  if (!request.ok()) {
    return request.error();
  }

  auto state = std::make_shared<StreamHandle::State>();
  auto worker = std::jthread([this, request = std::move(request.value()), state, &observer,
                              method = std::string(operation.method),
                              endpoint = std::string(operation.endpoint)]() mutable {
    SseParser parser;

    const auto stream_response = stream_requester_(
        request,
        [&parser, &observer, state](std::string_view chunk) -> core::Result<void> {
          if (state->cancel_requested.load()) {
            return {};
          }
          return parser.Feed(chunk, [&observer](const SseEvent& event) {
            return DispatchSseEvent(event, observer);
          });
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

    const auto version_check = ValidateResponseVersion(stream_response.value());
    if (!version_check.ok()) {
      NotifyErrorAndStop(*state, observer, version_check.error());
      return;
    }

    if (stream_response.value().status_code < kHttpOkMin ||
        stream_response.value().status_code > kHttpOkMax) {
      NotifyErrorAndStop(*state, observer,
                         BuildHttpError(method, endpoint, stream_response.value()));
      return;
    }

    const auto finish = parser.Finish(
        [&observer](const SseEvent& event) { return DispatchSseEvent(event, observer); });
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
