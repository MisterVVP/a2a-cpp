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
constexpr const char* kMissingRequester = "HTTP requester is not configured";
constexpr std::string_view kDefaultMtlsUnsupportedMessage =
    "default libcurl HTTP requester does not support mTLS options; inject a custom requester for mTLS";
}  // namespace
using http_json_internal::MakeHttpRequesterForClient;
using http_json_internal::PrepareCancelTask;
using http_json_internal::PrepareCreateTaskPushNotificationConfig;
using http_json_internal::PrepareDeleteTaskPushNotificationConfig;
using http_json_internal::PrepareGetTask;
using http_json_internal::PrepareGetTaskPushNotificationConfig;
using http_json_internal::PrepareListTaskPushNotificationConfigs;
using http_json_internal::PrepareListTasks;
using http_json_internal::PrepareSendMessage;
using http_json_internal::ToClientHttpResponse;
using http_json_internal::ToSharedHttpRequest;
using http_json_internal::ValidateResponseVersion;

HttpRequester MakeDefaultHttpRequester() { return MakeHttpRequesterForClient(std::make_shared<http::Client>()); }

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     HttpStreamRequester stream_requester, std::chrono::milliseconds default_timeout,
                                     HttpAsyncRequester async_requester)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      stream_requester_(std::move(stream_requester)),
      default_timeout_(default_timeout) {
  async_transport_ =
      http_json_internal::MakeAsyncTransport(resolved_interface_, std::move(async_requester), default_timeout_);
}

HttpJsonTransport::HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                     std::chrono::milliseconds default_timeout)
    : HttpJsonTransport(std::move(resolved_interface), std::move(requester), HttpStreamRequester{}, default_timeout) {}

std::unique_ptr<HttpJsonTransport> HttpJsonTransport::CreateDefault(ResolvedInterface resolved_interface,
                                                                    std::chrono::milliseconds default_timeout) {
  auto default_http_client = std::make_shared<a2a::http::Client>();
  auto async_requester = [client = default_http_client](
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
  auto transport = std::make_unique<HttpJsonTransport>(
      std::move(resolved_interface), MakeHttpRequesterForClient(default_http_client), HttpStreamRequester{},
      default_timeout, std::move(async_requester));
  transport->default_async_stream_client_ = std::move(default_http_client);
  return transport;
}

core::Result<HttpRequest> HttpJsonTransport::BuildUnaryRequest(HttpOperation operation, std::string body,
                                                               const CallOptions& options) const {
  return http_json_internal::BuildUnaryRequest(resolved_interface_, default_timeout_, operation, std::move(body),
                                               options);
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

}  // namespace a2a::client
