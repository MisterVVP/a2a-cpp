// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

#include "a2a/client/http_json_transport.h"
#include "a2a/core/error.h"
#include "a2a/core/protocol_methods.h"
#include "a2a/http/http_client.h"

namespace a2a::client::http_json_internal {

struct EndpointMap final {
  static constexpr std::string_view kSendMessage = "/message:send";
  static constexpr std::string_view kSendStreamingMessage = "/message:stream";
  static constexpr std::string_view kTaskCollection = "/tasks";
  static constexpr std::string_view kPushConfigCollection = core::protocol_methods::kPushNotificationConfigsSegment;
};

[[nodiscard]] a2a::http::Request ToSharedHttpRequest(const HttpRequest& request);
[[nodiscard]] HttpClientResponse ToClientHttpResponse(a2a::http::Response response);

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] std::string JoinUrl(std::string_view interface_base_url, std::string_view rpc_endpoint);
[[nodiscard]] core::Result<void> ValidateResponseVersion(const HttpClientResponse& response);
[[nodiscard]] core::Error BuildHttpError(std::string_view method, std::string_view endpoint,
                                         const HttpClientResponse& response);
[[nodiscard]] std::string BuildTaskPath(std::string_view task_id);

[[nodiscard]] core::Result<HttpRequest> BuildUnaryRequest(const ResolvedInterface& resolved_interface,
                                                          std::chrono::milliseconds default_timeout,
                                                          HttpOperation operation, std::string body,
                                                          const CallOptions& options);
[[nodiscard]] std::shared_ptr<AsyncClientTransport> MakeAsyncTransport(ResolvedInterface resolved_interface,
                                                                       HttpAsyncRequester requester,
                                                                       std::chrono::milliseconds default_timeout);

template <typename T>
struct PreparedUnary final {
  std::string_view method;
  std::string endpoint;
  std::string body;
  core::Result<T> (*decode)(const HttpClientResponse&, std::string_view);
};
[[nodiscard]] HttpRequester MakeHttpRequesterForClient(std::shared_ptr<http::Client> client);
[[nodiscard]] core::Result<PreparedUnary<lf::a2a::v1::SendMessageResponse>> PrepareSendMessage(
    const lf::a2a::v1::SendMessageRequest& request);
[[nodiscard]] core::Result<PreparedUnary<lf::a2a::v1::Task>> PrepareGetTask(const lf::a2a::v1::GetTaskRequest& request);
[[nodiscard]] core::Result<PreparedUnary<ListTasksResponse>> PrepareListTasks(const ListTasksRequest& request);
[[nodiscard]] core::Result<PreparedUnary<lf::a2a::v1::Task>> PrepareCancelTask(
    const lf::a2a::v1::CancelTaskRequest& request);
[[nodiscard]] core::Result<PreparedUnary<lf::a2a::v1::TaskPushNotificationConfig>>
PrepareCreateTaskPushNotificationConfig(const lf::a2a::v1::TaskPushNotificationConfig& request);
[[nodiscard]] core::Result<PreparedUnary<lf::a2a::v1::TaskPushNotificationConfig>> PrepareGetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request);
[[nodiscard]] core::Result<PreparedUnary<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
PrepareListTaskPushNotificationConfigs(const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request);
[[nodiscard]] core::Result<PreparedUnary<void>> PrepareDeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request);

}  // namespace a2a::client::http_json_internal
