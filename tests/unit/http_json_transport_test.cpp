// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/http_json_transport.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "a2a/client/auth.h"
#include "a2a/client/client.h"
#include "a2a/core/error.h"

namespace {
using a2a::client::A2AClient;
using a2a::client::CallOptions;
using a2a::client::HttpClientResponse;
using a2a::client::HttpJsonTransport;
using a2a::client::HttpRequest;
using a2a::client::PreferredTransport;
using a2a::client::ResolvedInterface;
using a2a::core::ErrorCode;

constexpr int kHttpOk = 200;
constexpr int kHttpNoContent = 204;
constexpr int kHistoryLength = 9;
constexpr std::string_view kTaskId = "task-1";
constexpr std::string_view kPushConfigId = "cfg-1";
constexpr std::string_view kWebhookUrl = "https://callback.example.test/push";
constexpr int kPushListPageSize = 1;
constexpr std::size_t kPushCrudRequestCount = 4U;
constexpr std::string_view kTaskScopedPushCollectionUrl =
    "https://agent.example.com/a2a/tasks/task-1/pushNotificationConfigs";
constexpr std::string_view kTaskScopedPushConfigUrl =
    "https://agent.example.com/a2a/tasks/task-1/pushNotificationConfigs/cfg-1";
constexpr std::string_view kTaskScopedPushListUrl =
    "https://agent.example.com/a2a/tasks/task-1/pushNotificationConfigs?pageSize=1";

ResolvedInterface MakeResolvedRest() {
  ResolvedInterface resolved;
  resolved.transport = PreferredTransport::kRest;
  resolved.url = "https://agent.example.com/a2a/";
  return resolved;
}

HttpClientResponse BuildPushCrudResponse(const HttpRequest& request) {
  if (request.method == "DELETE") {
    return HttpClientResponse{.status_code = kHttpNoContent, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
  }
  if (request.method == "GET" && request.url.find("pageSize") != std::string::npos) {
    return HttpClientResponse{.status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = R"({"configs":[]})"};
  }
  return HttpClientResponse{.status_code = kHttpOk,
                            .headers = {{"A2A-Version", "1.0"}},
                            .body = R"({"id":"cfg-1","taskId":"task-1","url":"https://callback.example.test/push"})"};
}

void ExercisePushCrud(A2AClient& client) {
  lf::a2a::v1::TaskPushNotificationConfig create_request;
  create_request.set_task_id(std::string(kTaskId));
  create_request.set_url(std::string(kWebhookUrl));
  ASSERT_TRUE(client.CreateTaskPushNotificationConfig(create_request).ok());

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_task_id(std::string(kTaskId));
  get_request.set_id(std::string(kPushConfigId));
  ASSERT_TRUE(client.GetTaskPushNotificationConfig(get_request).ok());

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  list_request.set_task_id(std::string(kTaskId));
  list_request.set_page_size(kPushListPageSize);
  ASSERT_TRUE(client.ListTaskPushNotificationConfigs(list_request).ok());

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_task_id(std::string(kTaskId));
  delete_request.set_id(std::string(kPushConfigId));
  ASSERT_TRUE(client.DeleteTaskPushNotificationConfig(delete_request).ok());
}

std::vector<std::string> CapturePushCrudRequestUrls() {
  std::vector<std::string> captured_urls;
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [&captured_urls](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured_urls.push_back(request.url);
        return BuildPushCrudResponse(request);
      });

  A2AClient client(std::move(transport));
  ExercisePushCrud(client);
  return captured_urls;
}

TEST(HttpJsonTransportUnitTest, GetTaskBuildsExpectedRequest) {
  HttpRequest captured;
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = R"({"id":"task-1"})"};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("task-1");
  request.set_history_length(kHistoryLength);
  const auto response = client.GetTask(request);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(captured.method, "GET");
  EXPECT_EQ(captured.url, "https://agent.example.com/a2a/tasks/task-1?historyLength=9");
  EXPECT_EQ(captured.headers.at("Accept"), "application/json");
}

TEST(HttpJsonTransportUnitTest, ListTasksParsesResponseAndQuery) {
  HttpRequest captured;
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = R"({"tasks":[{"id":"task-1"}],"nextPageToken":"next"})"};
      });

  A2AClient client(std::move(transport));
  const auto response = client.ListTasks({.page_size = 3, .page_token = "cursor"});
  ASSERT_TRUE(response.ok());
  ASSERT_EQ(response.value().tasks.size(), 1U);
  EXPECT_EQ(response.value().tasks[0].id(), "task-1");
  EXPECT_EQ(response.value().next_page_token, "next");
  EXPECT_EQ(captured.url, "https://agent.example.com/a2a/tasks?pageSize=3&pageToken=cursor");
}

TEST(HttpJsonTransportUnitTest, PushNotificationConfigCrudBuildsTaskScopedRestPaths) {
  const std::vector<std::string> captured_urls = CapturePushCrudRequestUrls();
  const std::array<std::string_view, kPushCrudRequestCount> expected_urls = {
      kTaskScopedPushCollectionUrl, kTaskScopedPushConfigUrl, kTaskScopedPushListUrl, kTaskScopedPushConfigUrl};

  ASSERT_EQ(captured_urls.size(), expected_urls.size());
  for (std::size_t index = 0; index < expected_urls.size(); ++index) {
    EXPECT_EQ(captured_urls[index], expected_urls[index]);
  }
}

TEST(HttpJsonTransportUnitTest, PushNotificationConfigCrudRequiresTaskId) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Internal("requester should not be called");
      });
  A2AClient client(std::move(transport));

  lf::a2a::v1::TaskPushNotificationConfig create_request;
  create_request.set_url(std::string(kWebhookUrl));
  EXPECT_FALSE(client.CreateTaskPushNotificationConfig(create_request).ok());

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_id(std::string(kPushConfigId));
  EXPECT_FALSE(client.GetTaskPushNotificationConfig(get_request).ok());

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  EXPECT_FALSE(client.ListTaskPushNotificationConfigs(list_request).ok());

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_id(std::string(kPushConfigId));
  EXPECT_FALSE(client.DeleteTaskPushNotificationConfig(delete_request).ok());
}

TEST(HttpJsonTransportUnitTest, UnsupportedVersionHeaderReturnsError) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "999.0"}}, .body = R"({"id":"task-1"})"};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("task-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kUnsupportedVersion);
  EXPECT_EQ(response.error().transport().value_or(""), "http");
}

TEST(HttpJsonTransportUnitTest, ListTasksRejectsWrongNextPageTokenType) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = R"({"tasks":[],"nextPageToken":3})"};
      });

  A2AClient client(std::move(transport));
  const auto response = client.ListTasks({});
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kSerialization);
}

TEST(HttpJsonTransportUnitTest, GetTaskAppliesCredentialProvider) {
  HttpRequest captured;
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = R"({"id":"task-1"})"};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("task-1");
  CallOptions options;
  options.credential_provider = std::make_shared<a2a::client::BearerTokenCredentialProvider>("token-1");
  const auto response = client.GetTask(request, options);
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(captured.headers.at("Authorization"), "Bearer token-1");
}

}  // namespace
