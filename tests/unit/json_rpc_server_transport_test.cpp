// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/json_rpc_server_transport.h"

#include <google/protobuf/struct.pb.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "a2a/core/protojson.h"

namespace {

constexpr int kHttpOk = 200;
constexpr int kJsonRpcInternalError = -32603;
constexpr std::string_view kRpcPath = "/rpc";
constexpr std::string_view kA2aVersionHeader = "A2A-Version";
constexpr std::string_view kA2aVersionValue = "1.0";
constexpr std::string_view kJsonContentTypeHeader = "Content-Type";
constexpr std::string_view kTextPlainContentType = "text/plain";
constexpr std::string_view kApplicationJsonWithCharset = "Application/JSON; charset=utf-8";
constexpr std::string_view kTaskPushId = "push-task";
constexpr std::string_view kPushConfigId = "push-config";
constexpr std::string_view kWebhookUrl = "https://example.test/push";
constexpr std::size_t kDefaultListTasksPageSize = 50U;

class JsonRpcEchoExecutor final : public a2a::server::AgentExecutor {
 public:
  class SingleEventSession final : public a2a::server::ServerStreamSession {
   public:
    explicit SingleEventSession(lf::a2a::v1::StreamResponse event) : event_(std::move(event)) {}

    a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
      if (consumed_) {
        return std::optional<lf::a2a::v1::StreamResponse>{};
      }
      consumed_ = true;
      return std::optional<lf::a2a::v1::StreamResponse>(event_);
    }

   private:
    lf::a2a::v1::StreamResponse event_;
    bool consumed_ = false;
  };

  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& context) override {
    last_version_header = context.client_headers["A2A-Version"];
    last_bearer_token = context.auth_metadata["bearer_token"];
    lf::a2a::v1::SendMessageResponse response;
    response.mutable_task()->set_id(request.message().task_id());
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    if (fail_streaming) {
      return a2a::core::Error::Internal("stream unavailable");
    }
    (void)context;
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(request.message().task_id());
    return std::unique_ptr<a2a::server::ServerStreamSession>(std::make_unique<SingleEventSession>(event));
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    task.mutable_status()->set_state(task_state);
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)context;
    a2a::server::ListTasksResponse response;
    response.page_size = request.page_size;
    response.next_page_token = std::to_string(request.page_size);
    return response;
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, a2a::server::RequestContext& context) override {
    (void)context;
    last_push_task_id = request.task_id();
    lf::a2a::v1::TaskPushNotificationConfig response = request;
    response.set_id(std::string(kPushConfigId));
    return response;
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::TaskPushNotificationConfig response;
    response.set_id(request.id());
    response.set_task_id(request.task_id());
    response.set_url(std::string(kWebhookUrl));
    return response;
  }

  a2a::core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
    auto* config = response.add_configs();
    config->set_id(std::string(kPushConfigId));
    config->set_task_id(request.task_id());
    config->set_url(std::string(kWebhookUrl));
    return response;
  }

  a2a::core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    last_deleted_push_config_id = request.id();
    return {};
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Internal("cancel unavailable");
  }

  std::string last_version_header;
  std::string last_bearer_token;
  std::string last_push_task_id;
  std::string last_deleted_push_config_id;
  bool fail_streaming = false;
  lf::a2a::v1::TaskState task_state = lf::a2a::v1::TASK_STATE_WORKING;
};

a2a::server::HttpServerRequest BuildJsonRpcRequest(std::string body) {
  return {.method = "POST",
          .target = std::string(kRpcPath),
          .headers = {{std::string(kA2aVersionHeader), std::string(kA2aVersionValue)}},
          .body = std::move(body),
          .remote_address = {}};
}

TEST(JsonRpcServerTransportTest, HandlesSendMessageEnvelope) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body =
           R"({"jsonrpc":"2.0","id":"req-1","method":"a2a.sendMessage","params":{"message":{"role":"ROLE_USER","taskId":"task-1"}}})",
       .remote_address = "127.0.0.1"});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(executor.last_version_header, "1.0");
  EXPECT_NE(response.value().body.find("\"id\":\"req-1\""), std::string::npos);
  EXPECT_NE(response.value().body.find("task-1"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsMalformedEnvelope) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle({.method = "POST",
                                       .target = "/rpc",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = "{not json",
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("-32700"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsMissingMethod) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle({.method = "POST",
                                       .target = "/rpc",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = R"({"jsonrpc":"2.0","id":"req-2","params":{}})",
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("method must be a non-empty string"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsInvalidParamsShape) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response =
      server.Handle({.method = "POST",
                     .target = "/rpc",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = R"({"jsonrpc":"2.0","id":"req-3","method":"a2a.getTask","params":[1,2,3]})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("params must be an object"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, ReturnsMethodNotFoundError) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle({
      .method = "POST",
      .target = "/rpc",
      .headers = {{"A2A-Version", "1.0"}},
      .body = R"({"jsonrpc":"2.0","id":"req-4","method":"a2a.noop","params":{}})",
      .remote_address = {},
  });

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_FALSE(response.value().body.empty());
}

TEST(JsonRpcServerTransportTest, MapsExecutorFailureToJsonRpcError) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response =
      server.Handle({.method = "POST",
                     .target = "/rpc",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = R"({"jsonrpc":"2.0","id":"req-5","method":"a2a.cancelTask","params":{"id":"task-1"}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);

  google::protobuf::Struct envelope;
  ASSERT_TRUE(a2a::core::JsonToMessage(response.value().body, &envelope).ok());
  ASSERT_TRUE(envelope.fields().contains("error"));
  const auto& error_fields = envelope.fields().at("error").struct_value().fields();
  EXPECT_EQ(static_cast<int>(error_fields.at("code").number_value()), kJsonRpcInternalError);
  EXPECT_EQ(error_fields.at("message").string_value(), "cancel unavailable");
}

TEST(JsonRpcServerTransportTest, ExtractsAuthMetadataIntoRequestContext) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}, {"Authorization", "Bearer token-rpc"}},
       .body =
           R"({"jsonrpc":"2.0","id":"req-auth","method":"a2a.sendMessage","params":{"message":{"role":"ROLE_USER","taskId":"task-auth"}}})",
       .remote_address = "127.0.0.1"});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(executor.last_bearer_token, "token-rpc");
}

TEST(JsonRpcServerTransportTest, SupportsLegacyTasksListMethodAlias) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response =
      server.Handle({.method = "POST",
                     .target = "/rpc",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = R"({"jsonrpc":"2.0","id":"req-list","method":"tasks/list","params":{"pageSize":10}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("\"result\""), std::string::npos);
}

TEST(JsonRpcServerTransportTest, ListTasksUsesDefaultPageSizeWhenOmitted) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(
      &dispatcher, {.rpc_path = "/rpc", .default_list_tasks_page_size = kDefaultListTasksPageSize});

  const auto response =
      server.Handle({.method = "POST",
                     .target = "/rpc",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = R"({"jsonrpc":"2.0","id":"req-list-default","method":"tasks/list","params":{}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("\"nextPageToken\":\"50\""), std::string::npos);
}

TEST(JsonRpcServerTransportTest, UnknownRouteUsesMethodNotFoundCode) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response =
      server.Handle({.method = "POST",
                     .target = "/not-rpc",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = R"({"jsonrpc":"2.0","id":"req-route","method":"a2a.listTasks","params":{}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_FALSE(response.value().body.empty());
}

TEST(JsonRpcServerTransportTest, ListTasksInvalidPageSizeReturnsInvalidParams) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body = R"({"jsonrpc":"2.0","id":"req-list-invalid","method":"tasks/list","params":{"pageSize":0}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("-32602"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsUnsupportedContentType) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response =
      server.Handle({.method = "POST",
                     .target = "/rpc",
                     .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/plain"}},
                     .body = R"({"jsonrpc":"2.0","id":"req-content","method":"a2a.getTask","params":{"id":"task-1"}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_FALSE(response.value().body.empty());
}

TEST(JsonRpcServerTransportTest, RejectsInvalidJsonRpcVersion) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response =
      server.Handle({.method = "POST",
                     .target = "/rpc",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = R"({"jsonrpc":"1.0","id":"req-version","method":"a2a.getTask","params":{"id":"task-1"}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_FALSE(response.value().body.empty());
}

TEST(JsonRpcServerTransportTest, RejectsInvalidIdType) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response =
      server.Handle({.method = "POST",
                     .target = "/rpc",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = R"({"jsonrpc":"2.0","id":{"nested":1},"method":"a2a.getTask","params":{"id":"task-1"}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_FALSE(response.value().body.empty());
}

TEST(JsonRpcServerTransportTest, RejectsMissingProtocolVersionHeaderWhenConfigured) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc", .require_version_header = true});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"Content-Type", "application/json"}},
       .body = R"({"jsonrpc":"2.0","id":"req-no-version","method":"a2a.getTask","params":{"id":"task-1"}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("-32009"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, SupportsStreamingMethodWithSseResponse) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body =
           R"({"jsonrpc":"2.0","id":"req-stream","method":"a2a.sendStreamingMessage","params":{"message":{"role":"ROLE_USER","taskId":"task-stream"}}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(response.value().headers.at("Content-Type"), "text/event-stream");
  EXPECT_NE(response.value().body.find("task-stream"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, SubscribeToTaskReturnsSseEventsForNonTerminalTask) {
  JsonRpcEchoExecutor executor;
  executor.task_state = lf::a2a::v1::TASK_STATE_WORKING;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body = R"({"jsonrpc":"2.0","id":"req-sub","method":"a2a.subscribeToTask","params":{"id":"task-sub"}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(response.value().headers.at("Content-Type"), "text/event-stream");
  EXPECT_NE(response.value().body.find("task-sub"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, SubscribeToTaskRejectsTerminalTask) {
  JsonRpcEchoExecutor executor;
  executor.task_state = lf::a2a::v1::TASK_STATE_COMPLETED;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body = R"({"jsonrpc":"2.0","id":"req-sub-terminal","method":"a2a.subscribeToTask","params":{"id":"task-sub"}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("task is already terminal"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsInvalidListTasksValues) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body = R"({"jsonrpc":"2.0","id":"req-list-token","method":"a2a.listTasks","params":{"pageToken":"abc"}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("pageToken must be a valid offset"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsPlainTextContentTypeWithProtocolErrorReason) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = std::string(kRpcPath)});

  const auto response =
      server.Handle({.method = "POST",
                     .target = std::string(kRpcPath),
                     .headers = {{std::string(kJsonContentTypeHeader), std::string(kTextPlainContentType)}},
                     .body = R"({"jsonrpc":"2.0","id":"req-type","method":"a2a.getTask","params":{"id":"task-1"}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("CONTENT_TYPE_NOT_SUPPORTED"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, AcceptsJsonContentTypeWithDifferentCasing) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = std::string(kRpcPath)});

  const auto response =
      server.Handle({.method = "POST",
                     .target = std::string(kRpcPath),
                     .headers = {{std::string(kA2aVersionHeader), std::string(kA2aVersionValue)},
                                 {std::string(kJsonContentTypeHeader), std::string(kApplicationJsonWithCharset)}},
                     .body = R"({"jsonrpc":"2.0","id":"req-type-ok","method":"a2a.getTask","params":{"id":"task-1"}})",
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("task-1"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsUnsupportedVersionHeaderValue) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = std::string(kRpcPath)});

  const auto response = server.Handle(
      {.method = "POST",
       .target = std::string(kRpcPath),
       .headers = {{std::string(kA2aVersionHeader), "0.1"}},
       .body = R"({"jsonrpc":"2.0","id":"req-version-header","method":"a2a.getTask","params":{"id":"task-1"}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("VERSION_NOT_SUPPORTED"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, ParsesListTasksFilters) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = std::string(kRpcPath)});

  const auto response = server.Handle(BuildJsonRpcRequest(
      R"({"jsonrpc":"2.0","id":"req-list-filters","method":"a2a.listTasks","params":{"pageSize":3,"pageToken":"12","context_id":"ctx-1","status":"TASK_STATE_WORKING","statusTimestampAfter":"2026-01-02T03:04:05Z","historyLength":2,"includeArtifacts":true}})"));

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_NE(response.value().body.find("\"pageSize\":3"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsInvalidListTaskFilterTypes) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = std::string(kRpcPath)});

  const auto context_response = server.Handle(BuildJsonRpcRequest(
      R"({"jsonrpc":"2.0","id":"req-list-context","method":"a2a.listTasks","params":{"contextId":7}})"));
  ASSERT_TRUE(context_response.ok());
  EXPECT_NE(context_response.value().body.find("contextId must be a string"), std::string::npos);

  const auto status_response = server.Handle(BuildJsonRpcRequest(
      R"({"jsonrpc":"2.0","id":"req-list-status","method":"a2a.listTasks","params":{"status":"BOGUS"}})"));
  ASSERT_TRUE(status_response.ok());
  EXPECT_NE(status_response.value().body.find("status must be a valid"), std::string::npos);

  const auto include_response = server.Handle(BuildJsonRpcRequest(
      R"({"jsonrpc":"2.0","id":"req-list-artifacts","method":"a2a.listTasks","params":{"includeArtifacts":"yes"}})"));
  ASSERT_TRUE(include_response.ok());
  EXPECT_NE(include_response.value().body.find("includeArtifacts must be a boolean"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, HandlesPushNotificationConfigMethods) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = std::string(kRpcPath)});

  const auto create_response = server.Handle(BuildJsonRpcRequest(
      R"({"jsonrpc":"2.0","id":"req-push-create","method":"a2a.setTaskPushNotificationConfig","params":{"taskId":"push-task","pushNotificationConfig":{"url":"https://example.test/push"}}})"));
  ASSERT_TRUE(create_response.ok());
  EXPECT_EQ(executor.last_push_task_id, kTaskPushId);
  EXPECT_NE(create_response.value().body.find(std::string(kPushConfigId)), std::string::npos);

  const auto get_response = server.Handle(BuildJsonRpcRequest(
      R"({"jsonrpc":"2.0","id":"req-push-get","method":"a2a.getTaskPushNotificationConfig","params":{"taskId":"push-task","id":"push-config"}})"));
  ASSERT_TRUE(get_response.ok());
  EXPECT_NE(get_response.value().body.find(std::string(kWebhookUrl)), std::string::npos);

  const auto list_response = server.Handle(BuildJsonRpcRequest(
      R"({"jsonrpc":"2.0","id":"req-push-list","method":"a2a.listTaskPushNotificationConfigs","params":{"taskId":"push-task"}})"));
  ASSERT_TRUE(list_response.ok());
  EXPECT_NE(list_response.value().body.find(std::string(kPushConfigId)), std::string::npos);

  const auto delete_response = server.Handle(BuildJsonRpcRequest(
      R"({"jsonrpc":"2.0","id":"req-push-delete","method":"a2a.deleteTaskPushNotificationConfig","params":{"taskId":"push-task","id":"push-config"}})"));
  ASSERT_TRUE(delete_response.ok());
  EXPECT_EQ(executor.last_deleted_push_config_id, kPushConfigId);
  EXPECT_NE(delete_response.value().body.find("\"result\""), std::string::npos);
}

}  // namespace
