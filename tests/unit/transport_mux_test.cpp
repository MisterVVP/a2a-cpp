// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/transport_mux.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

using a2a::server::HttpServerRequest;
using a2a::server::HttpServerResponse;
using a2a::server::TransportMux;

constexpr int kHttpOk = 200;
constexpr int kHttpCreated = 201;
constexpr int kHttpInternalServerError = 500;
constexpr std::string_view kRpcPath = "/rpc";
constexpr std::string_view kNoRouteCode = "ROUTE_NOT_FOUND";
constexpr std::string_view kCustomNotFoundBody = "custom not found";
constexpr int kLowPriority = 1;
constexpr int kHighPriority = 10;
constexpr std::string_view kHistoryQueryTarget = "/a2a/tasks/task-1?historyLength=0";
constexpr std::string_view kPushTaskId = "push-task";
constexpr std::string_view kPushConfigId = "push-config";
constexpr std::string_view kWebhookUrl = "https://example.test/push";

class JsonRpcPushConfigRecordingExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return lf::a2a::v1::SendMessageResponse{};
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return std::unique_ptr<a2a::server::ServerStreamSession>{};
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return lf::a2a::v1::Task{};
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::server::ListTasksResponse{};
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return lf::a2a::v1::Task{};
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, a2a::server::RequestContext& context) override {
    (void)context;
    last_push_task_id = request.task_id();
    last_push_url = request.url();

    lf::a2a::v1::TaskPushNotificationConfig response = request;
    response.set_id(std::string(kPushConfigId));
    return response;
  }

  std::string last_push_task_id;
  std::string last_push_url;
};

TEST(TransportMuxTest, RoutesByPriorityAndNormalizesRootToDefaultPath) {
  TransportMux mux(
      {.normalization_policy = TransportMux::PathNormalizationPolicy::kRootToDefaultPath, .default_path = "/rpc"});
  mux.RegisterRoute(
      {.name = "low",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "POST" || method == "*") && path == "/rpc"; },
       .handler =
           [](const HttpServerRequest&) {
             HttpServerResponse response;
             response.status_code = kHttpCreated;
             return response;
           },
       .priority = kLowPriority});
  mux.RegisterRoute(
      {.name = "high",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "POST" || method == "*") && path == "/rpc"; },
       .handler =
           [](const HttpServerRequest&) {
             HttpServerResponse response;
             response.status_code = kHttpOk;
             return response;
           },
       .priority = kHighPriority});

  HttpServerRequest request{.method = "POST", .target = "/", .headers = {}, .body = "", .remote_address = ""};
  auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpOk);
}

TEST(TransportMuxTest, PreservesQueryStringWhenForwardingNormalizedTarget) {
  TransportMux mux;
  mux.RegisterRoute({.name = "rest",
                     .matcher =
                         [](std::string_view method, std::string_view path) {
                           return (method == "GET" || method == "*") && path == "/a2a/tasks/task-1";
                         },
                     .handler =
                         [](const HttpServerRequest& routed_request) {
                           HttpServerResponse response;
                           response.status_code =
                               routed_request.target == kHistoryQueryTarget ? kHttpOk : kHttpInternalServerError;
                           return response;
                         },
                     .priority = kLowPriority});

  HttpServerRequest request{
      .method = "GET", .target = "/a2a/tasks/task-1/?historyLength=0", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpOk);
}

TEST(TransportMuxTest, NormalizesRelativeTargetsAndTrimsTrailingSlash) {
  TransportMux mux;
  mux.RegisterRoute(
      {.name = "rpc",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "POST" || method == "*") && path == kRpcPath; },
       .handler =
           [](const HttpServerRequest& routed_request) {
             HttpServerResponse response;
             response.status_code = routed_request.target == kRpcPath ? kHttpOk : kHttpInternalServerError;
             return response;
           },
       .priority = kLowPriority});

  const HttpServerRequest request{.method = "POST", .target = "rpc/", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpOk);
}

TEST(TransportMuxTest, JsonRpcCreatePushConfigPreservesNestedWebhookUrl) {
  JsonRpcPushConfigRecordingExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = std::string(kRpcPath)});

  const auto response = server.Handle(
      {.method = "POST",
       .target = std::string(kRpcPath),
       .headers = {{"A2A-Version", "1.0"}},
       .body =
           R"({"jsonrpc":"2.0","id":"req-push-create","method":"a2a.setTaskPushNotificationConfig","params":{"taskId":"push-task","pushNotificationConfig":{"url":"https://example.test/push"}}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(executor.last_push_task_id, kPushTaskId);
  EXPECT_EQ(executor.last_push_url, kWebhookUrl);
  EXPECT_NE(response.value().body.find(std::string(kPushConfigId)), std::string::npos);
}

TEST(TransportMuxTest, ReturnsStructuredRouteMissForMissingPath) {
  TransportMux mux;

  const HttpServerRequest request{.method = "GET", .target = "", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, TransportMux::kHttpNotFound);
  EXPECT_NE(result.value().body.find(std::string(kNoRouteCode)), std::string::npos);
  EXPECT_EQ(mux.last_route_miss().normalized_path, "/");
}

TEST(TransportMuxTest, UsesCustomNotFoundHandler) {
  TransportMux mux;
  mux.SetNotFoundHandler([](const HttpServerRequest&) {
    HttpServerResponse response;
    response.status_code = kHttpInternalServerError;
    response.body = std::string(kCustomNotFoundBody);
    return response;
  });

  const HttpServerRequest request{
      .method = "GET", .target = "/missing", .headers = {}, .body = "", .remote_address = ""};
  const auto result = mux.RouteRequest(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, kHttpInternalServerError);
  EXPECT_EQ(result.value().body, kCustomNotFoundBody);
}

TEST(TransportMuxTest, ReturnsStructuredRouteMissForMethodMismatch) {
  TransportMux mux;
  mux.RegisterRoute(
      {.name = "rpc",
       .matcher = [](std::string_view method,
                     std::string_view path) { return (method == "POST" || method == "*") && path == "/rpc"; },
       .handler =
           [](const HttpServerRequest&) {
             HttpServerResponse response;
             response.status_code = kHttpOk;
             return response;
           },
       .priority = kLowPriority});

  HttpServerRequest request{.method = "GET", .target = "/rpc", .headers = {}, .body = "", .remote_address = ""};
  auto result = mux.RouteRequest(request);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().status_code, TransportMux::kHttpMethodNotAllowed);
  EXPECT_NE(result.value().body.find("ROUTE_METHOD_NOT_ALLOWED"), std::string::npos);
}

}  // namespace
