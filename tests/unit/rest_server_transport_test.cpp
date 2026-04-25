#include "a2a/server/rest_server_transport.h"

#include <gtest/gtest.h>

#include <string>

#include "a2a/core/protojson.h"

namespace {

class EchoExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    observed_request_header = context.client_headers["A2A-Version"];
    observed_bearer_token = context.auth_metadata["bearer_token"];
    observed_api_key = context.auth_metadata["api_key"];
    lf::a2a::v1::SendMessageResponse response;
    response.mutable_message()->set_task_id(request.message().task_id());
    response.mutable_message()->set_role("assistant");
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
    observed_history_length = request.history_length();
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(
      const a2a::server::ListTasksRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::server::ListTasksResponse{};
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return task;
  }

  std::string observed_request_header;
  std::string observed_history_length;
  std::string observed_bearer_token;
  std::string observed_api_key;
};

lf::a2a::v1::AgentCard BuildCard() {
  lf::a2a::v1::AgentCard card;
  card.set_name("Unit Agent");
  auto* iface = card.add_supported_interfaces();
  iface->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_REST);
  iface->set_url("http://localhost:8080/a2a");
  return card;
}

TEST(RestServerTransportTest, ServesAgentCardFromWellKnownEndpoint) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle({.method = "GET",
                                       .target = "/.well-known/agent-card.json",
                                       .headers = {},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(response.value().headers.at("A2A-Version"), "1.0");

  lf::a2a::v1::AgentCard parsed;
  ASSERT_TRUE(a2a::core::JsonToMessage(response.value().body, &parsed).ok());
  EXPECT_EQ(parsed.protocol_version(), "1.0");
  ASSERT_EQ(parsed.supported_interfaces_size(), 1);
  EXPECT_EQ(parsed.supported_interfaces(0).url(), "http://localhost:8080/a2a");
}

TEST(RestServerTransportTest, RoutesRequestUsingConfiguredBasePath) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle({.method = "POST",
                                       .target = "/a2a/messages:send",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = R"({"message":{"role":"user","taskId":"t-1"}})",
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(executor.observed_request_header, "1.0");
  EXPECT_NE(response.value().body.find("t-1"), std::string::npos);
}

TEST(RestServerTransportTest, RejectsMissingVersionWhenConfigured) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle({.method = "GET",
                                       .target = "/a2a/tasks/task-7",
                                       .headers = {},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 426);
  EXPECT_NE(response.value().body.find("Missing required A2A-Version header"), std::string::npos);
}

TEST(RestServerTransportTest, ParsesAndDecodesQueryString) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle({.method = "GET",
                                       .target = "/a2a/tasks/task-3?historyLength=alpha%20beta",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(executor.observed_history_length, "alpha beta");
}

TEST(RestServerTransportTest, ExtractsAuthMetadataIntoRequestContext) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle({.method = "POST",
                                       .target = "/a2a/messages:send",
                                       .headers = {{"A2A-Version", "1.0"},
                                                   {"Authorization", "Bearer token-rest"},
                                                   {"X-API-Key", "rest-key"}},
                                       .body = R"({"message":{"role":"user","taskId":"t-2"}})",
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(executor.observed_bearer_token, "token-rest");
  EXPECT_EQ(executor.observed_api_key, "rest-key");
}

}  // namespace
