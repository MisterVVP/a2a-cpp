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
#include "request_headers.h"

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
constexpr const char* kGetTaskIdRequired = "GetTaskRequest.id is required";
constexpr const char* kCancelTaskIdRequired = "CancelTaskRequest.id is required";
constexpr const char* kGetPushConfigIdRequired = "GetTaskPushNotificationConfigRequest.id is required";
constexpr const char* kDeletePushConfigIdRequired = "DeleteTaskPushNotificationConfigRequest.id is required";
constexpr const char* kCreatePushTaskIdRequired = "TaskPushNotificationConfig.task_id is required";
constexpr const char* kGetPushTaskIdRequired = "GetTaskPushNotificationConfigRequest.task_id is required";
constexpr const char* kListPushTaskIdRequired = "ListTaskPushNotificationConfigsRequest.task_id is required";
constexpr const char* kDeletePushTaskIdRequired = "DeleteTaskPushNotificationConfigRequest.task_id is required";
constexpr const char* kMissingRequester = "HTTP requester is not configured";
constexpr const char* kMissingAsyncRequester = "HTTP async requester is not configured";
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

template <typename T>
struct PreparedUnary final {
  std::string_view method;
  std::string endpoint;
  std::string body;
  core::Result<T> (*decode)(const HttpClientResponse&, std::string_view);
};
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
    endpoint += "?historyLength=" + std::to_string(request.history_length());
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

  std::string endpoint = BuildTaskPath(request.id()) + ":cancel";
  return PreparedUnary<lf::a2a::v1::Task>{
      .method = core::http::kMethodPost,
      .endpoint = std::move(endpoint),
      .body = "{}",
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

        if (response.status_code != kHttpNoContent && !response.body.empty() && response.body != "{}") {
          google::protobuf::Empty ignored;
          const auto parse = core::JsonToMessage(response.body, &ignored);
          if (!parse.ok()) {
            return parse.error().WithTransport("http").WithHttpStatus(response.status_code);
          }
        }

        return {};
      }};
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
                                     HttpStreamRequester stream_requester, std::chrono::milliseconds default_timeout,
                                     HttpAsyncRequester async_requester)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      async_requester_(std::move(async_requester)),
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
  transport->async_requester_ =
      [client = default_http_client](
          const HttpRequest& request,
          core::Completion<HttpClientResponse> completion) -> core::Result<core::AsyncHandle> {
    if (request.mtls) {
      return core::Error::Validation(std::string(kDefaultMtlsUnsupportedMessage));
    }
    return client->SendRequestAsync(ToSharedHttpRequest(request),
                                    [completion = std::move(completion)](core::Result<http::Response> response) {
                                      if (!response.ok()) {
                                        completion(response.error());
                                      } else {
                                        completion(ToClientHttpResponse(std::move(response.value())));
                                      }
                                    });
  };
  transport->default_async_stream_client_ = std::move(default_http_client);
  return transport;
}

core::Result<HttpRequest> HttpJsonTransport::BuildUnaryRequest(HttpOperation operation, std::string body,
                                                               const CallOptions& options) const {
  if (resolved_interface_.transport != PreferredTransport::kRest) {
    return core::Error::Validation("HttpJsonTransport requires a REST interface");
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

core::Result<HttpClientResponse> HttpJsonTransport::SendRequest(HttpOperation operation, std::string body,
                                                                const CallOptions& options) const {
  if (!requester_) {
    return core::Error::Internal(kMissingRequester);
  }
  const auto prepared = BuildUnaryRequest(operation, std::move(body), options);
  if (!prepared.ok()) {
    return prepared.error();
  }
  const auto response = requester_(prepared.value());
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
  auto prepared = PrepareSendMessage(request);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto& plan = prepared.value();
  auto response = SendRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!response.ok()) {
    return response.error();
  }
  return plan.decode(response.value(), plan.endpoint);
}
void HttpJsonTransport::StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
                                         const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) {
  auto prepared = PrepareSendMessage(request);
  if (!prepared.ok()) {
    completion.Complete(prepared.error());
    return;
  }
  auto plan = std::move(prepared.value());
  auto http_request =
      BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!http_request.ok()) {
    completion.Complete(http_request.error());
    return;
  }
  if (!async_requester_) {
    completion.Complete(core::Error::Validation(kMissingAsyncRequester));
    return;
  }
  auto started =
      async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                 core::Result<HttpClientResponse> response) {
        if (!response.ok()) {
          completion.Complete(response.error());
          return;
        }
        const auto version = ValidateResponseVersion(response.value());
        if (!version.ok()) {
          completion.Complete(version.error());
          return;
        }
        completion.Complete(decode(response.value(), endpoint));
      });
  if (!started.ok()) {
    completion.Complete(started.error());
    return;
  }
  completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
}

core::Result<lf::a2a::v1::Task> HttpJsonTransport::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                           const CallOptions& options) {
  auto prepared = PrepareGetTask(request);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto& plan = prepared.value();
  auto response = SendRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!response.ok()) {
    return response.error();
  }
  return plan.decode(response.value(), plan.endpoint);
}
void HttpJsonTransport::StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                                     const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  auto prepared = PrepareGetTask(request);
  if (!prepared.ok()) {
    completion.Complete(prepared.error());
    return;
  }
  auto plan = std::move(prepared.value());
  auto http_request =
      BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!http_request.ok()) {
    completion.Complete(http_request.error());
    return;
  }
  if (!async_requester_) {
    completion.Complete(core::Error::Validation(kMissingAsyncRequester));
    return;
  }
  auto started =
      async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                 core::Result<HttpClientResponse> response) {
        if (!response.ok()) {
          completion.Complete(response.error());
          return;
        }
        const auto version = ValidateResponseVersion(response.value());
        if (!version.ok()) {
          completion.Complete(version.error());
          return;
        }
        completion.Complete(decode(response.value(), endpoint));
      });
  if (!started.ok()) {
    completion.Complete(started.error());
    return;
  }
  completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
}

core::Result<ListTasksResponse> HttpJsonTransport::ListTasks(const ListTasksRequest& request,
                                                             const CallOptions& options) {
  auto prepared = PrepareListTasks(request);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto& plan = prepared.value();
  auto response = SendRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!response.ok()) {
    return response.error();
  }
  return plan.decode(response.value(), plan.endpoint);
}
void HttpJsonTransport::StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                                       const core::AsyncCompletion<ListTasksResponse>& completion) {
  auto prepared = PrepareListTasks(request);
  if (!prepared.ok()) {
    completion.Complete(prepared.error());
    return;
  }
  auto plan = std::move(prepared.value());
  auto http_request =
      BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!http_request.ok()) {
    completion.Complete(http_request.error());
    return;
  }
  if (!async_requester_) {
    completion.Complete(core::Error::Validation(kMissingAsyncRequester));
    return;
  }
  auto started =
      async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                 core::Result<HttpClientResponse> response) {
        if (!response.ok()) {
          completion.Complete(response.error());
          return;
        }
        const auto version = ValidateResponseVersion(response.value());
        if (!version.ok()) {
          completion.Complete(version.error());
          return;
        }
        completion.Complete(decode(response.value(), endpoint));
      });
  if (!started.ok()) {
    completion.Complete(started.error());
    return;
  }
  completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
}

core::Result<lf::a2a::v1::Task> HttpJsonTransport::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                              const CallOptions& options) {
  auto prepared = PrepareCancelTask(request);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto& plan = prepared.value();
  auto response = SendRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!response.ok()) {
    return response.error();
  }
  return plan.decode(response.value(), plan.endpoint);
}
void HttpJsonTransport::StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                                        const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  auto prepared = PrepareCancelTask(request);
  if (!prepared.ok()) {
    completion.Complete(prepared.error());
    return;
  }
  auto plan = std::move(prepared.value());
  auto http_request =
      BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!http_request.ok()) {
    completion.Complete(http_request.error());
    return;
  }
  if (!async_requester_) {
    completion.Complete(core::Error::Validation(kMissingAsyncRequester));
    return;
  }
  auto started =
      async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                 core::Result<HttpClientResponse> response) {
        if (!response.ok()) {
          completion.Complete(response.error());
          return;
        }
        const auto version = ValidateResponseVersion(response.value());
        if (!version.ok()) {
          completion.Complete(version.error());
          return;
        }
        completion.Complete(decode(response.value(), endpoint));
      });
  if (!started.ok()) {
    completion.Complete(started.error());
    return;
  }
  completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> HttpJsonTransport::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  auto prepared = PrepareCreateTaskPushNotificationConfig(request);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto& plan = prepared.value();
  auto response = SendRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!response.ok()) {
    return response.error();
  }
  return plan.decode(response.value(), plan.endpoint);
}
void HttpJsonTransport::StartCreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  auto prepared = PrepareCreateTaskPushNotificationConfig(request);
  if (!prepared.ok()) {
    completion.Complete(prepared.error());
    return;
  }
  auto plan = std::move(prepared.value());
  auto http_request =
      BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!http_request.ok()) {
    completion.Complete(http_request.error());
    return;
  }
  if (!async_requester_) {
    completion.Complete(core::Error::Validation(kMissingAsyncRequester));
    return;
  }
  auto started =
      async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                 core::Result<HttpClientResponse> response) {
        if (!response.ok()) {
          completion.Complete(response.error());
          return;
        }
        const auto version = ValidateResponseVersion(response.value());
        if (!version.ok()) {
          completion.Complete(version.error());
          return;
        }
        completion.Complete(decode(response.value(), endpoint));
      });
  if (!started.ok()) {
    completion.Complete(started.error());
    return;
  }
  completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> HttpJsonTransport::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  auto prepared = PrepareGetTaskPushNotificationConfig(request);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto& plan = prepared.value();
  auto response = SendRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!response.ok()) {
    return response.error();
  }
  return plan.decode(response.value(), plan.endpoint);
}
void HttpJsonTransport::StartGetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  auto prepared = PrepareGetTaskPushNotificationConfig(request);
  if (!prepared.ok()) {
    completion.Complete(prepared.error());
    return;
  }
  auto plan = std::move(prepared.value());
  auto http_request =
      BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!http_request.ok()) {
    completion.Complete(http_request.error());
    return;
  }
  if (!async_requester_) {
    completion.Complete(core::Error::Validation(kMissingAsyncRequester));
    return;
  }
  auto started =
      async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                 core::Result<HttpClientResponse> response) {
        if (!response.ok()) {
          completion.Complete(response.error());
          return;
        }
        const auto version = ValidateResponseVersion(response.value());
        if (!version.ok()) {
          completion.Complete(version.error());
          return;
        }
        completion.Complete(decode(response.value(), endpoint));
      });
  if (!started.ok()) {
    completion.Complete(started.error());
    return;
  }
  completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> HttpJsonTransport::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) {
  auto prepared = PrepareListTaskPushNotificationConfigs(request);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto& plan = prepared.value();
  auto response = SendRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!response.ok()) {
    return response.error();
  }
  return plan.decode(response.value(), plan.endpoint);
}
void HttpJsonTransport::StartListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) {
  auto prepared = PrepareListTaskPushNotificationConfigs(request);
  if (!prepared.ok()) {
    completion.Complete(prepared.error());
    return;
  }
  auto plan = std::move(prepared.value());
  auto http_request =
      BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!http_request.ok()) {
    completion.Complete(http_request.error());
    return;
  }
  if (!async_requester_) {
    completion.Complete(core::Error::Validation(kMissingAsyncRequester));
    return;
  }
  auto started =
      async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                 core::Result<HttpClientResponse> response) {
        if (!response.ok()) {
          completion.Complete(response.error());
          return;
        }
        const auto version = ValidateResponseVersion(response.value());
        if (!version.ok()) {
          completion.Complete(version.error());
          return;
        }
        completion.Complete(decode(response.value(), endpoint));
      });
  if (!started.ok()) {
    completion.Complete(started.error());
    return;
  }
  completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
}

core::Result<void> HttpJsonTransport::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  auto prepared = PrepareDeleteTaskPushNotificationConfig(request);
  if (!prepared.ok()) {
    return prepared.error();
  }
  auto& plan = prepared.value();
  auto response = SendRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!response.ok()) {
    return response.error();
  }
  return plan.decode(response.value(), plan.endpoint);
}
void HttpJsonTransport::StartDeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<void>& completion) {
  auto prepared = PrepareDeleteTaskPushNotificationConfig(request);
  if (!prepared.ok()) {
    completion.Complete(prepared.error());
    return;
  }
  auto plan = std::move(prepared.value());
  auto http_request =
      BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
  if (!http_request.ok()) {
    completion.Complete(http_request.error());
    return;
  }
  if (!async_requester_) {
    completion.Complete(core::Error::Validation(kMissingAsyncRequester));
    return;
  }
  auto started =
      async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                 core::Result<HttpClientResponse> response) {
        if (!response.ok()) {
          completion.Complete(response.error());
          return;
        }
        const auto version = ValidateResponseVersion(response.value());
        if (!version.ok()) {
          completion.Complete(version.error());
          return;
        }
        completion.Complete(decode(response.value(), endpoint));
      });
  if (!started.ok()) {
    completion.Complete(started.error());
    return;
  }
  completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
}

}  // namespace a2a::client
