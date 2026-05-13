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
    response.mutable_message()->set_role(lf::a2a::v1::ROLE_AGENT);
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
  int observed_history_length = -1;
  std::string observed_bearer_token;
  std::string observed_api_key;
};

lf::a2a::v1::AgentCard BuildCard() {
  lf::a2a::v1::AgentCard card;
  card.set_name("Unit Agent");
  card.set_description("Unit test agent");
  card.set_version("1.0.0");
  card.add_default_input_modes("text/plain");
  card.add_default_output_modes("text/plain");
  auto* iface = card.add_supported_interfaces();
  iface->set_protocol_binding("HTTP+JSON");
  iface->set_protocol_version("1.0");
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
  ASSERT_FALSE(parsed.supported_interfaces().empty());
  EXPECT_EQ(parsed.supported_interfaces(0).protocol_version(), "1.0");
  ASSERT_EQ(parsed.supported_interfaces_size(), 1);
  EXPECT_EQ(parsed.supported_interfaces(0).url(), "http://localhost:8080/a2a");
}

TEST(RestServerTransportTest, ServesAgentCardFromLegacyWellKnownEndpoint) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle({.method = "GET",
                                       .target = "/.well-known/agent.json",
                                       .headers = {},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
}

TEST(RestServerTransportTest, AddsBackwardCompatibleTransportFieldsToAgentCard) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle({.method = "GET",
                                       .target = "/.well-known/agent-card.json",
                                       .headers = {},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  google::protobuf::Struct parsed;
  ASSERT_TRUE(a2a::core::JsonToMessage(response.value().body, &parsed).ok());
  const auto& fields = parsed.fields();
  ASSERT_TRUE(fields.contains("endpoint"));
  EXPECT_EQ(fields.at("endpoint").string_value(), "http://localhost:8080/a2a");
  ASSERT_TRUE(fields.contains("preferredTransport"));
  EXPECT_EQ(fields.at("preferredTransport").string_value(), "rest");
  ASSERT_TRUE(fields.contains("additionalInterfaces"));
  EXPECT_TRUE(fields.at("additionalInterfaces").has_list_value());
}

TEST(RestServerTransportTest, RoutesRequestUsingConfiguredBasePath) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/a2a/messages:send",
       .headers = {{"A2A-Version", "1.0"}},
       .body =
           R"({"message":{"messageId":"msg-1","role":"ROLE_USER","parts":[{"text":"hello"}],"taskId":"t-1"}})",
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
                                       .target = "/a2a/tasks/task-3?historyLength=20",
                                       .headers = {{"A2A-Version", "1.0"}},
                                       .body = {},
                                       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(executor.observed_history_length, 20);
}

TEST(RestServerTransportTest, ExtractsAuthMetadataIntoRequestContext) {
  EchoExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle(
      {.method = "POST",
       .target = "/a2a/messages:send",
       .headers = {{"A2A-Version", "1.0"},
                   {"Authorization", "Bearer token-rest"},
                   {"X-API-Key", "rest-key"}},
       .body =
           R"({"message":{"messageId":"msg-2","role":"ROLE_USER","parts":[{"text":"hello"}],"taskId":"t-2"}})",
       .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 200);
  EXPECT_EQ(executor.observed_bearer_token, "token-rest");
  EXPECT_EQ(executor.observed_api_key, "rest-key");
}

}  // namespace
