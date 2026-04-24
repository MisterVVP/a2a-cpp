#include <gtest/gtest.h>

#include "../support/rest_server_test_utils.h"
#include "a2a/core/protojson.h"
#include "a2a/server/rest_server_transport.h"

namespace {

TEST(RestServerTransportFunctionalTest, SupportsCoreTaskLifecycleOverHttpTargetMapping) {
  a2a::server::InMemoryTaskStore store;
  a2a::tests::support::StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher,
      a2a::tests::support::BuildRestAgentCard("Functional REST Agent", "http://localhost:9090/api"),
      {.rest_api_base_path = "/api"});

  const auto send_response = server.Handle(a2a::tests::support::MakeHttpRequest(
      "POST", "/api/messages:send", {{"A2A-Version", "1.0"}},
      R"({"message":{"role":"user","taskId":"task-functional-1"}})"));
  ASSERT_TRUE(send_response.ok());
  EXPECT_EQ(send_response.value().status_code, 200);

  const auto list_response = server.Handle(a2a::tests::support::MakeHttpRequest(
      "GET", "/api/tasks?pageSize=10", {{"A2A-Version", "1.0"}}));
  ASSERT_TRUE(list_response.ok());
  EXPECT_EQ(list_response.value().status_code, 200);
  EXPECT_NE(list_response.value().body.find("task-functional-1"), std::string::npos);

  const auto cancel_response = server.Handle(a2a::tests::support::MakeHttpRequest(
      "POST", "/api/tasks/task-functional-1:cancel", {{"A2A-Version", "1.0"}}, "{}"));
  ASSERT_TRUE(cancel_response.ok());
  EXPECT_EQ(cancel_response.value().status_code, 200);

  lf::a2a::v1::Task canceled_task;
  ASSERT_TRUE(a2a::core::JsonToMessage(cancel_response.value().body, &canceled_task).ok());
  EXPECT_EQ(canceled_task.status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

TEST(RestServerTransportFunctionalTest, ReturnsStructuredNotFoundForMalformedInput) {
  a2a::server::InMemoryTaskStore store;
  a2a::tests::support::StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher,
      a2a::tests::support::BuildRestAgentCard("Functional REST Agent", "http://localhost:9090/api"),
      {.rest_api_base_path = "/api"});

  const auto response = server.Handle(a2a::tests::support::MakeHttpRequest(
      "GET", "/api/tasks?pageSize=abc", {{"A2A-Version", "1.0"}}));

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 404);
  EXPECT_NE(response.value().body.find("validation_error"), std::string::npos);
}

}  // namespace
