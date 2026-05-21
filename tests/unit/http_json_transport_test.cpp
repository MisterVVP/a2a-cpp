#include "a2a/client/http_json_transport.h"

#include <gtest/gtest.h>

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

ResolvedInterface MakeResolvedRest() {
  ResolvedInterface resolved;
  resolved.transport = PreferredTransport::kRest;
  resolved.url = "https://agent.example.com/a2a/";
  return resolved;
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

TEST(HttpJsonTransportUnitTest, DeleteTaskPushNotificationConfigHandlesNoContent) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        EXPECT_EQ(request.method, "DELETE");
        return HttpClientResponse{.status_code = kHttpNoContent, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request;
  request.set_id("cfg-1");
  const auto response = client.DeleteTaskPushNotificationConfig(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
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
