#include "a2a/client/http_json_transport.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>

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
std::string JoinUrl(std::string_view interface_base_url,
                    std::string_view rpc_endpoint) {
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

}  // namespace

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     std::chrono::milliseconds default_timeout)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      default_timeout_(default_timeout) {}

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

  if (!options.extensions.empty()) {
    request.headers[std::string(core::Extensions::kHeaderName)] = core::Extensions::Format(options.extensions);
  }

  if (options.auth_hook) {
    options.auth_hook(request.headers);
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
  const auto response = SendRequest({.method = "POST", .endpoint = endpoint}, body.value(), options);
  if (!response.ok()) {
    return response.error();
  }

  return ParseBodyOrMapError<lf::a2a::v1::SendMessageResponse>("POST", endpoint, response.value());
}

core::Result<lf::a2a::v1::Task> HttpJsonTransport::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                           const CallOptions& options) {
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

core::Result<lf::a2a::v1::TaskPushNotificationConfig> HttpJsonTransport::SetTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  const auto body = core::MessageToJson(request);
  if (!body.ok()) {
    return body.error();
  }

  const std::string endpoint(EndpointMap::kPushConfigCollection);
  const auto response = SendRequest({.method = "POST", .endpoint = endpoint}, body.value(), options);
  if (!response.ok()) {
    return response.error();
  }
  return ParseBodyOrMapError<lf::a2a::v1::TaskPushNotificationConfig>("POST", endpoint,
                                                                       response.value());
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> HttpJsonTransport::GetTaskPushNotificationConfig(
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
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) {
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
  return ParseBodyOrMapError<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>("GET", path,
                                                                                    response.value());
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

}  // namespace a2a::client
