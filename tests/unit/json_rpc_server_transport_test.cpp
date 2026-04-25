#include "a2a/server/json_rpc_server_transport.h"

#include <google/protobuf/struct.pb.h>
#include <gtest/gtest.h>

#include <string>

#include "a2a/core/protojson.h"

namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpInternalServerError = 500;
constexpr int kJsonRpcInternalError = -32603;

class JsonRpcEchoExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    last_version_header = context.client_headers["A2A-Version"];
    last_bearer_token = context.auth_metadata["bearer_token"];
    lf::a2a::v1::SendMessageResponse response;
    response.mutable_task()->set_id(request.message().task_id());
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(
      const a2a::server::ListTasksRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    a2a::server::ListTasksResponse response;
    response.next_page_token = std::to_string(request.page_size);
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Internal("cancel unavailable");
  }

  std::string last_version_header;
  std::string last_bearer_token;
};

TEST(JsonRpcServerTransportTest, HandlesSendMessageEnvelope) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body =
           R"({"jsonrpc":"2.0","id":"req-1","method":"a2a.sendMessage","params":{"message":{"role":"user","taskId":"task-1"}}})",
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
  EXPECT_EQ(response.value().status_code, kHttpBadRequest);
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
  EXPECT_EQ(response.value().status_code, kHttpBadRequest);
  EXPECT_NE(response.value().body.find("method must be a non-empty string"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, RejectsInvalidParamsShape) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body = R"({"jsonrpc":"2.0","id":"req-3","method":"a2a.getTask","params":[1,2,3]})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpBadRequest);
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
  EXPECT_EQ(response.value().status_code, kHttpBadRequest);
  EXPECT_NE(response.value().body.find("-32601"), std::string::npos);
}

TEST(JsonRpcServerTransportTest, MapsExecutorFailureToJsonRpcError) {
  JsonRpcEchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/rpc",
       .headers = {{"A2A-Version", "1.0"}},
       .body =
           R"({"jsonrpc":"2.0","id":"req-5","method":"a2a.cancelTask","params":{"id":"task-1"}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpInternalServerError);

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
           R"({"jsonrpc":"2.0","id":"req-auth","method":"a2a.sendMessage","params":{"message":{"role":"user","taskId":"task-auth"}}})",
       .remote_address = "127.0.0.1"});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, kHttpOk);
  EXPECT_EQ(executor.last_bearer_token, "token-rpc");
}

}  // namespace
