// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/error.h"
#include "a2a/core/protocol_methods.h"

namespace {

using a2a::client::A2AClient;
using a2a::client::CallOptions;
using a2a::client::HeaderMap;
using a2a::client::HttpClientResponse;
using a2a::client::HttpJsonTransport;
using a2a::client::HttpRequest;
using a2a::client::ResolvedInterface;
using a2a::core::ErrorCode;

constexpr int kHttpOk = 200;
constexpr int kHttpNoContent = 204;
constexpr int kHttpBadGateway = 502;
constexpr int kListTasksPageSize = 25;
constexpr int kPageSize = 25;
constexpr int kCustomTimeoutMs = 2500;
constexpr std::string_view kTaskId = "t-1";
constexpr std::string_view kPushConfigId = "pn-1";

ResolvedInterface MakeResolvedRest() {
  ResolvedInterface resolved;
  resolved.transport = a2a::client::PreferredTransport::kRest;
  resolved.url = "https://agent.example.com/a2a";
  return resolved;
}

HttpClientResponse MakeOkResponse(std::string body) {
  return {.status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = std::move(body)};
}

void ExpectSendMessageRequest(const HttpRequest& request) {
  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.url, "https://agent.example.com/a2a/message:send");
  EXPECT_EQ(request.timeout, std::chrono::milliseconds(kCustomTimeoutMs));
  EXPECT_EQ(request.headers.at("A2A-Version"), "1.0");
  EXPECT_EQ(request.headers.at("Content-Type"), "application/json");
}

void ExpectSendMessageHeadersAndBody(const HttpRequest& request) {
  EXPECT_EQ(request.headers.at("Accept"), "application/json");
  EXPECT_EQ(request.headers.at("X-Request-Id"), "abc123");
  EXPECT_EQ(request.headers.at("Authorization"), "Bearer token");
  EXPECT_EQ(request.headers.at("A2A-Extensions"), "ext.alpha,ext.beta");
  EXPECT_NE(request.body.find("\"role\":\"ROLE_USER\""), std::string::npos);
}

CallOptions MakeCallOptions() {
  CallOptions options;
  options.timeout = std::chrono::milliseconds(kCustomTimeoutMs);
  options.headers.emplace("X-Request-Id", "abc123");
  options.extensions = {"ext.beta", "ext.alpha"};
  options.auth_hook = [](HeaderMap& headers) { headers["Authorization"] = "Bearer token"; };
  return options;
}

TEST(HttpJsonClientIntegrationTest, SendMessageHappyPathSetsHeadersAndParsesResponse) {
  HttpRequest captured;
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = R"({"message":{"role":"ROLE_AGENT"}})"};
      });
  A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  const auto response = client.SendMessage(request, MakeCallOptions());
  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_TRUE(response.value().has_message());
  EXPECT_EQ(response.value().message().role(), lf::a2a::v1::ROLE_AGENT);

  ExpectSendMessageRequest(captured);
  ExpectSendMessageHeadersAndBody(captured);
}

a2a::core::Result<HttpClientResponse> HandleGetAndCancelRequest(int& call, const HttpRequest& request) {
  ++call;
  if (call == 1) {
    EXPECT_EQ(request.method, "GET");
    EXPECT_EQ(request.url, "https://agent.example.com/a2a/tasks/t-1?historyLength=2");
    return MakeOkResponse(R"({"id":"t-1"})");
  }
  EXPECT_EQ(request.method, "POST");
  EXPECT_EQ(request.url, "https://agent.example.com/a2a/tasks/t-1:cancel");
  EXPECT_EQ(request.body, "{}");
  return MakeOkResponse(R"({"id":"t-1","status":{"state":"TASK_STATE_CANCELED"}})");
}

TEST(HttpJsonClientIntegrationTest, GetTaskAndCancelTaskHappyPath) {
  int call = 0;
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [&call](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        return HandleGetAndCancelRequest(call, request);
      });
  A2AClient client(std::move(transport));

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("t-1");
  get_request.set_history_length(2);
  const auto get_response = client.GetTask(get_request);
  ASSERT_TRUE(get_response.ok()) << get_response.error().message();
  EXPECT_EQ(get_response.value().id(), "t-1");

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id("t-1");
  const auto cancel_response = client.CancelTask(cancel_request);
  ASSERT_TRUE(cancel_response.ok()) << cancel_response.error().message();
  EXPECT_EQ(cancel_response.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

a2a::core::Result<HttpClientResponse> HandleListTasksRequest(const HttpRequest& request) {
  EXPECT_EQ(request.method, "GET");
  EXPECT_EQ(request.url, "https://agent.example.com/a2a/tasks?pageSize=25&pageToken=cursor-1");
  return MakeOkResponse(R"({"tasks":[{"id":"t-1"},{"id":"t-2"}],"nextPageToken":"cursor-2"})");
}

TEST(HttpJsonClientIntegrationTest, ListTasksBuildsQueryAndParsesTasks) {
  auto transport = std::make_unique<HttpJsonTransport>(MakeResolvedRest(), HandleListTasksRequest);
  A2AClient client(std::move(transport));

  a2a::client::ListTasksRequest request{.page_size = kListTasksPageSize, .page_token = "cursor-1"};
  const auto response = client.ListTasks(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_EQ(response.value().tasks.size(), 2U);
  EXPECT_EQ(response.value().tasks[0].id(), "t-1");
  EXPECT_EQ(response.value().next_page_token, "cursor-2");
}

bool IsPushConfigCollectionRequest(const HttpRequest& request, std::string_view method) {
  std::string suffix = "/tasks/t-1";
  suffix.append(a2a::core::protocol_methods::kPushNotificationConfigsSegment);
  return request.method == method && request.url.ends_with(suffix);
}

bool IsPushConfigItemRequest(const HttpRequest& request, std::string_view method) {
  std::string suffix = "/tasks/t-1";
  suffix.append(a2a::core::protocol_methods::kPushNotificationConfigsSegment);
  suffix.append("/pn-1");
  return request.method == method && request.url.ends_with(suffix);
}

bool IsPushConfigListRequest(const HttpRequest& request) {
  std::string path = "/tasks/t-1";
  path.append(a2a::core::protocol_methods::kPushNotificationConfigsSegment);
  path.append("?pageSize=25");
  return request.method == "GET" && request.url.find(path) != std::string::npos;
}

a2a::core::Result<HttpClientResponse> HandlePushConfigRequest(const HttpRequest& request) {
  if (IsPushConfigCollectionRequest(request, "POST")) {
    return MakeOkResponse(R"({"id":"pn-1","taskId":"t-1","url":"https://cb"})");
  }
  if (IsPushConfigItemRequest(request, "GET")) {
    return MakeOkResponse(R"({"id":"pn-1","taskId":"t-1","url":"https://cb"})");
  }
  if (IsPushConfigListRequest(request)) {
    return MakeOkResponse(R"({"configs":[{"id":"pn-1","taskId":"t-1"}]})");
  }
  if (IsPushConfigItemRequest(request, "DELETE")) {
    return HttpClientResponse{.status_code = kHttpNoContent, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
  }
  return a2a::core::Error::Internal("unexpected request");
}

void VerifyPushConfigCrud(A2AClient& client) {
  lf::a2a::v1::TaskPushNotificationConfig set_request;
  set_request.set_task_id(kTaskId.data());
  set_request.set_url("https://cb");
  const auto set_response = client.CreateTaskPushNotificationConfig(set_request);
  ASSERT_TRUE(set_response.ok()) << set_response.error().message();
  EXPECT_EQ(set_response.value().id(), kPushConfigId);

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_task_id(kTaskId.data());
  get_request.set_id(kPushConfigId.data());
  const auto get_response = client.GetTaskPushNotificationConfig(get_request);
  ASSERT_TRUE(get_response.ok()) << get_response.error().message();
  EXPECT_EQ(get_response.value().task_id(), kTaskId);
}

void VerifyPushConfigListAndDelete(A2AClient& client) {
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  list_request.set_task_id(kTaskId.data());
  list_request.set_page_size(kPageSize);
  const auto list_response = client.ListTaskPushNotificationConfigs(list_request);
  ASSERT_TRUE(list_response.ok()) << list_response.error().message();
  ASSERT_EQ(list_response.value().configs_size(), 1);
  EXPECT_EQ(list_response.value().configs(0).id(), kPushConfigId);

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_task_id(kTaskId.data());
  delete_request.set_id(kPushConfigId.data());
  const auto delete_response = client.DeleteTaskPushNotificationConfig(delete_request);
  EXPECT_TRUE(delete_response.ok()) << delete_response.error().message();
}

TEST(HttpJsonClientIntegrationTest, SupportsPushNotificationConfigCrudAndList) {
  auto transport = std::make_unique<HttpJsonTransport>(MakeResolvedRest(), HandlePushConfigRequest);
  A2AClient client(std::move(transport));
  VerifyPushConfigCrud(client);
  VerifyPushConfigListAndDelete(client);
}

TEST(HttpJsonClientIntegrationTest, MapsRemote4xxAnd5xxIntoProtocolErrors) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpBadGateway,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = R"({"code":"UPSTREAM_FAILURE","message":"bad gateway"})"};
      });
  A2AClient client(std::move(transport));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kRemoteProtocol);
  ASSERT_TRUE(response.error().http_status().has_value());
  EXPECT_EQ(response.error().http_status().value_or(0), kHttpBadGateway);
  ASSERT_TRUE(response.error().protocol_code().has_value());
  EXPECT_EQ(response.error().protocol_code().value_or(""), "UPSTREAM_FAILURE");
}

TEST(HttpJsonClientIntegrationTest, InvalidJsonBodyMapsToSerializationError) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = "{broken json"};
      });
  A2AClient client(std::move(transport));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kSerialization);
  EXPECT_EQ(response.error().http_status().value_or(0), kHttpOk);
}

TEST(HttpJsonClientIntegrationTest, UnsupportedVersionResponseFailsFast) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "2.0"}}, .body = R"({"id":"t-1"})"};
      });
  A2AClient client(std::move(transport));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kUnsupportedVersion);
}

TEST(HttpJsonClientIntegrationTest, MissingEndpointMappingFromAgentCardReturnsValidationError) {
  ResolvedInterface resolved = MakeResolvedRest();
  resolved.url.clear();

  auto transport =
      std::make_unique<HttpJsonTransport>(resolved, [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk, .headers = {}, .body = R"({"id":"t-1"})"};
      });
  A2AClient client(std::move(transport));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kValidation);
}

}  // namespace
