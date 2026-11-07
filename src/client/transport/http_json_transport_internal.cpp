// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "http_json_transport_internal.h"

#include <google/protobuf/empty.pb.h>
#include <google/protobuf/struct.pb.h>

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/extensions.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "request_headers.h"

namespace a2a::client::http_json_internal {
namespace {

constexpr const char* kHttpTransportName = "http";
constexpr const char* kHistoryQuery = "?historyLength=";
constexpr const char* kPageSizeQuery = "pageSize=";
constexpr const char* kPageTokenQuery = "pageToken=";
constexpr const char* kCancelSuffix = ":cancel";
constexpr const char* kEmptyObject = "{}";
constexpr const char* kMissingRestInterface = "HttpJsonTransport requires a REST interface";
constexpr const char* kMissingRestUrl = "Resolved REST interface URL is required";
constexpr std::string_view kUnsupportedVersionMessage = "Server returned unsupported A2A-Version header";
constexpr std::string_view kHttpRequestFailurePrefix = "HTTP request failed for ";
constexpr std::string_view kProtocolCodeMemberName = "code";

}  // namespace

a2a::http::Request ToSharedHttpRequest(const HttpRequest& request) {
  a2a::http::Request shared_request;
  shared_request.tls = request.tls;
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
    base.push_back('/');
  }
  base.append(rpc_endpoint);
  return base;
}

core::Result<void> ValidateResponseVersion(const HttpClientResponse& response) {
  const auto version = core::http::FindHeaderValue(response.headers, core::Version::kHeaderName);
  if (!version.has_value()) {
    return {};
  }
  if (!core::Version::IsSupported(version.value())) {
    return core::Error::UnsupportedVersion(std::string(kUnsupportedVersionMessage))
        .WithTransport(kHttpTransportName)
        .WithProtocolCode(std::string(version.value()));
  }
  return {};
}

core::Error BuildHttpError(std::string_view method, std::string_view endpoint, const HttpClientResponse& response) {
  std::ostringstream stream;
  stream << kHttpRequestFailurePrefix << method << " " << endpoint;
  if (!response.body.empty()) {
    stream << ": " << response.body;
  }

  core::Error error = core::Error::RemoteProtocol(stream.str()).WithTransport(kHttpTransportName);
  error = error.WithHttpStatus(response.status_code);

  if (!response.body.empty() && response.body.front() == '{') {
    google::protobuf::Struct status_payload;
    if (core::JsonToMessage(response.body, &status_payload, {.ignore_unknown_fields = true}).ok()) {
      const auto code = status_payload.fields().find(kProtocolCodeMemberName);
      if (code != status_payload.fields().end() &&
          code->second.kind_case() == ::google::protobuf::Value::kStringValue) {
        error = error.WithProtocolCode(code->second.string_value());
      }
    }
  }
  return error;
}

std::string BuildTaskPath(std::string_view task_id) {
  std::string path;
  path.reserve(EndpointMap::kTaskCollection.size() + 1 + task_id.size());
  path += EndpointMap::kTaskCollection;
  path += '/';
  path += task_id;
  return path;
}

}  // namespace a2a::client::http_json_internal

namespace a2a::client::http_json_internal {

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr int kHttpNoContent = 204;
constexpr const char* kGetTaskIdRequired = "GetTaskRequest.id is required";
constexpr const char* kCancelTaskIdRequired = "CancelTaskRequest.id is required";
constexpr const char* kGetPushConfigIdRequired = "GetTaskPushNotificationConfigRequest.id is required";
constexpr const char* kDeletePushConfigIdRequired = "DeleteTaskPushNotificationConfigRequest.id is required";
constexpr const char* kCreatePushTaskIdRequired = "TaskPushNotificationConfig.task_id is required";
constexpr const char* kGetPushTaskIdRequired = "GetTaskPushNotificationConfigRequest.task_id is required";
constexpr const char* kListPushTaskIdRequired = "ListTaskPushNotificationConfigsRequest.task_id is required";
constexpr const char* kDeletePushTaskIdRequired = "DeleteTaskPushNotificationConfigRequest.task_id is required";
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
    return parse.error().WithTransport(kHttpTransportName).WithHttpStatus(response.status_code);
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
    return parse.error().WithTransport(kHttpTransportName).WithHttpStatus(response.status_code);
  }

  ListTasksResponse parsed;
  parsed.tasks.reserve(static_cast<std::size_t>(payload.tasks_size()));
  for (auto& task : *payload.mutable_tasks()) {
    parsed.tasks.push_back(std::move(task));
  }
  parsed.next_page_token = std::move(*payload.mutable_next_page_token());

  return parsed;
}

core::Result<PreparedUnary<lf::a2a::v1::SendMessageResponse>> PrepareSendMessage(
    const lf::a2a::v1::SendMessageRequest& request) {
  const auto body = core::MessageToJson(request);
  if (!body.ok()) {
    return body.error();
  }

  std::string endpoint(EndpointMap::kSendMessage);
  return PreparedUnary<lf::a2a::v1::SendMessageResponse>{
      .method = core::http::kMethodPost,
      .endpoint = std::move(endpoint),
      .body = body.value(),
      .decode = [](const HttpClientResponse& response,
                   std::string_view endpoint) -> core::Result<lf::a2a::v1::SendMessageResponse> {
        return ParseBodyOrMapError<lf::a2a::v1::SendMessageResponse>(core::http::kMethodPost, endpoint, response);
      }};
}
core::Result<PreparedUnary<lf::a2a::v1::Task>> PrepareGetTask(const lf::a2a::v1::GetTaskRequest& request) {
  if (request.id().empty()) {
    return core::Error::Validation(kGetTaskIdRequired);
  }

  std::string endpoint = BuildTaskPath(request.id());
  if (request.has_history_length()) {
    endpoint += kHistoryQuery + std::to_string(request.history_length());
  }

  return PreparedUnary<lf::a2a::v1::Task>{
      .method = core::http::kMethodGet,
      .endpoint = std::move(endpoint),
      .body = {},
      .decode = [](const HttpClientResponse& response, std::string_view endpoint) -> core::Result<lf::a2a::v1::Task> {
        return ParseBodyOrMapError<lf::a2a::v1::Task>(core::http::kMethodGet, endpoint, response);
      }};
}
core::Result<PreparedUnary<ListTasksResponse>> PrepareListTasks(const ListTasksRequest& request) {
  std::ostringstream endpoint;
  endpoint << EndpointMap::kTaskCollection;
  if (request.page_size > 0 || !request.page_token.empty()) {
    endpoint << '?';
    bool has_previous = false;
    if (request.page_size > 0) {
      endpoint << kPageSizeQuery << request.page_size;
      has_previous = true;
    }
    if (!request.page_token.empty()) {
      if (has_previous) {
        endpoint << '&';
      }
      endpoint << kPageTokenQuery << request.page_token;
    }
  }

  std::string endpoint_path = endpoint.str();
  return PreparedUnary<ListTasksResponse>{
      .method = core::http::kMethodGet,
      .endpoint = std::move(endpoint_path),
      .body = {},
      .decode = [](const HttpClientResponse& response, std::string_view endpoint_path)
          -> core::Result<ListTasksResponse> { return ParseListTasksResponsePayload(response, endpoint_path); }};
}
core::Result<PreparedUnary<lf::a2a::v1::Task>> PrepareCancelTask(const lf::a2a::v1::CancelTaskRequest& request) {
  if (request.id().empty()) {
    return core::Error::Validation(kCancelTaskIdRequired);
  }

  std::string endpoint = BuildTaskPath(request.id()) + kCancelSuffix;
  return PreparedUnary<lf::a2a::v1::Task>{
      .method = core::http::kMethodPost,
      .endpoint = std::move(endpoint),
      .body = kEmptyObject,
      .decode = [](const HttpClientResponse& response, std::string_view endpoint) -> core::Result<lf::a2a::v1::Task> {
        return ParseBodyOrMapError<lf::a2a::v1::Task>(core::http::kMethodPost, endpoint, response);
      }};
}
core::Result<PreparedUnary<lf::a2a::v1::TaskPushNotificationConfig>> PrepareCreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request) {
  if (request.task_id().empty()) {
    return core::Error::Validation(kCreatePushTaskIdRequired);
  }

  const auto body = core::MessageToJson(request);
  if (!body.ok()) {
    return body.error();
  }

  std::string endpoint = BuildTaskPushConfigCollectionPath(request.task_id());
  return PreparedUnary<lf::a2a::v1::TaskPushNotificationConfig>{
      .method = core::http::kMethodPost,
      .endpoint = std::move(endpoint),
      .body = body.value(),
      .decode = [](const HttpClientResponse& response,
                   std::string_view endpoint) -> core::Result<lf::a2a::v1::TaskPushNotificationConfig> {
        return ParseBodyOrMapError<lf::a2a::v1::TaskPushNotificationConfig>(core::http::kMethodPost, endpoint,
                                                                            response);
      }};
}
core::Result<PreparedUnary<lf::a2a::v1::TaskPushNotificationConfig>> PrepareGetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request) {
  if (request.task_id().empty()) {
    return core::Error::Validation(kGetPushTaskIdRequired);
  }
  if (request.id().empty()) {
    return core::Error::Validation(kGetPushConfigIdRequired);
  }

  std::string endpoint = BuildTaskPushConfigPath({.task_id = request.task_id(), .id = request.id()});
  return PreparedUnary<lf::a2a::v1::TaskPushNotificationConfig>{
      .method = core::http::kMethodGet,
      .endpoint = std::move(endpoint),
      .body = {},
      .decode = [](const HttpClientResponse& response,
                   std::string_view endpoint) -> core::Result<lf::a2a::v1::TaskPushNotificationConfig> {
        return ParseBodyOrMapError<lf::a2a::v1::TaskPushNotificationConfig>(core::http::kMethodGet, endpoint, response);
      }};
}
core::Result<PreparedUnary<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
PrepareListTaskPushNotificationConfigs(const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request) {
  if (request.task_id().empty()) {
    return core::Error::Validation(kListPushTaskIdRequired);
  }

  std::ostringstream endpoint;
  endpoint << BuildTaskPushConfigCollectionPath(request.task_id());
  if (request.page_size() > 0 || !request.page_token().empty()) {
    endpoint << '?';
    bool has_previous = false;
    if (request.page_size() > 0) {
      endpoint << kPageSizeQuery << request.page_size();
      has_previous = true;
    }
    if (!request.page_token().empty()) {
      if (has_previous) {
        endpoint << '&';
      }
      endpoint << kPageTokenQuery << request.page_token();
    }
  }

  std::string path = endpoint.str();
  return PreparedUnary<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>{
      .method = core::http::kMethodGet,
      .endpoint = std::move(path),
      .body = {},
      .decode = [](const HttpClientResponse& response,
                   std::string_view path) -> core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> {
        return ParseBodyOrMapError<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(core::http::kMethodGet, path,
                                                                                         response);
      }};
}
core::Result<PreparedUnary<void>> PrepareDeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request) {
  if (request.task_id().empty()) {
    return core::Error::Validation(kDeletePushTaskIdRequired);
  }
  if (request.id().empty()) {
    return core::Error::Validation(kDeletePushConfigIdRequired);
  }

  std::string endpoint = BuildTaskPushConfigPath({.task_id = request.task_id(), .id = request.id()});
  return PreparedUnary<void>{
      .method = core::http::kMethodDelete,
      .endpoint = std::move(endpoint),
      .body = {},
      .decode = [](const HttpClientResponse& response, std::string_view endpoint) -> core::Result<void> {
        if (response.status_code < kHttpOkMin || response.status_code > kHttpOkMax) {
          return BuildHttpError(core::http::kMethodDelete, endpoint, response);
        }

        if (response.status_code != kHttpNoContent && !response.body.empty() && response.body != kEmptyObject) {
          google::protobuf::Empty ignored;
          const auto parse = core::JsonToMessage(response.body, &ignored);
          if (!parse.ok()) {
            return parse.error().WithTransport(kHttpTransportName).WithHttpStatus(response.status_code);
          }
        }

        return {};
      }};
}

core::Result<HttpRequest> BuildUnaryRequest(const ResolvedInterface& resolved_interface,
                                            std::chrono::milliseconds default_timeout, HttpOperation operation,
                                            std::string body, const CallOptions& options) {
  if (resolved_interface.transport != PreferredTransport::kRest) {
    return core::Error::Validation(kMissingRestInterface);
  }
  if (resolved_interface.url.empty()) {
    return core::Error::Validation(kMissingRestUrl);
  }

  HttpRequest request;
  request.method = std::string(operation.method);
  request.url = JoinUrl(resolved_interface.url, operation.endpoint);
  request.body = std::move(body);
  request.timeout = options.timeout.value_or(default_timeout);

  request.headers = options.headers;
  request.headers[std::string(core::http::kContentTypeHeaderName)] =
      std::string(core::http::kContentTypeApplicationJson);
  request.headers[std::string(core::http::kAcceptHeaderName)] = std::string(core::http::kContentTypeApplicationJson);
  request.tls = options.tls;
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

  const auto version = internal::ApplyProtocolVersion(options, request.headers);
  if (!version.ok()) {
    return version.error();
  }

  return request;
}
}  // namespace a2a::client::http_json_internal
