// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/http_json_transport.h"

#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "a2a/http/http_client.h"
#include "http_json_transport_internal.h"

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
constexpr int kHttpNoContent = 204;
constexpr std::string_view kDefaultMtlsUnsupportedMessage =
    "default libcurl HTTP requester does not support mTLS options; inject a custom requester for mTLS";

HttpRequester MakeHttpRequesterForClient(std::shared_ptr<a2a::http::Client> client) {
  return [client = std::move(client)](const HttpRequest& request) -> core::Result<HttpClientResponse> {
    if (request.mtls.has_value()) {
      return core::Error::Validation(std::string(kDefaultMtlsUnsupportedMessage));
    }
    auto response = client->SendRequest(ToSharedHttpRequest(request));
    if (!response.ok()) {
      return response.error();
    }
    return ToClientHttpResponse(std::move(response.value()));
  };
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

core::Result<ListTasksResponse> ParseListTasksResponsePayload(const HttpClientResponse& response,
                                                              std::string_view endpoint) {
  if (response.status_code < kHttpOkMin || response.status_code > kHttpOkMax) {
    return BuildHttpError(core::http::kMethodGet, endpoint, response);
  }

  lf::a2a::v1::ListTasksResponse payload;
  const auto parse = core::JsonToMessage(
      response.body, &payload,
      {.ignore_unknown_fields = true, .reject_top_level_null_fields = true, .reject_duplicate_top_level_fields = true});
  if (!parse.ok()) {
    return parse.error().WithTransport("http").WithHttpStatus(response.status_code);
  }

  ListTasksResponse parsed;
  parsed.tasks.reserve(static_cast<std::size_t>(payload.tasks_size()));
  for (auto& task : *payload.mutable_tasks()) {
    parsed.tasks.push_back(std::move(task));
  }
  parsed.next_page_token = std::move(*payload.mutable_next_page_token());

  return parsed;
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

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     HttpStreamRequester stream_requester, std::chrono::milliseconds default_timeout)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      stream_requester_(std::move(stream_requester)),
      default_timeout_(default_timeout) {}

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     std::chrono::milliseconds default_timeout)
    : HttpJsonTransport(std::move(resolved_interface), std::move(requester), HttpStreamRequester{}, default_timeout) {}

std::unique_ptr<HttpJsonTransport> HttpJsonTransport::CreateDefault(ResolvedInterface resolved_interface,
                                                                    std::chrono::milliseconds default_timeout) {
  auto default_http_client = std::make_shared<a2a::http::Client>();
  auto transport = std::make_unique<HttpJsonTransport>(std::move(resolved_interface),
                                                       MakeHttpRequesterForClient(default_http_client),
                                                       HttpStreamRequester{}, default_timeout);
  transport->default_async_stream_client_ = std::move(default_http_client);
  return transport;
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
  request.headers[std::string(core::http::kContentTypeHeaderName)] =
      std::string(core::http::kContentTypeApplicationJson);
  request.headers[std::string(core::http::kAcceptHeaderName)] = std::string(core::http::kContentTypeApplicationJson);
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

}  // namespace a2a::client
