#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "a2a/core/protojson.h"
#include "a2a/server/rest_server_transport.h"

namespace {

class StoreExecutor final : public a2a::server::AgentExecutor {
 public:
  explicit StoreExecutor(a2a::server::TaskStore* store) : store_(store) {}

  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;

    lf::a2a::v1::Task task;
    task.set_id(request.message().task_id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    const auto store_result = store_->CreateOrUpdate(task);
    if (!store_result.ok()) {
      return store_result.error();
    }

    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task;
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("streaming disabled in this test");
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    return store_->Get(request.id());
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(
      const a2a::server::ListTasksRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    return store_->List(request);
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    return store_->Cancel(request.id());
  }

 private:
  a2a::server::TaskStore* store_;
};

lf::a2a::v1::AgentCard BuildCard() {
  lf::a2a::v1::AgentCard card;
  card.set_name("Functional REST Agent");
  auto* iface = card.add_supported_interfaces();
  iface->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_REST);
  iface->set_url("http://localhost:9090/api");
  return card;
}

TEST(RestServerTransportFunctionalTest, SupportsCoreTaskLifecycleOverHttpTargetMapping) {
  a2a::server::InMemoryTaskStore store;
  StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/api"});

  const auto send_response = server.Handle(
      {.method = "POST",
       .target = "/api/messages:send",
       .headers = {{"A2A-Version", "1.0"}},
       .body = R"({"message":{"role":"user","taskId":"task-functional-1"}})",
       .remote_address = {}});
  ASSERT_TRUE(send_response.ok());
  EXPECT_EQ(send_response.value().status_code, 200);

  const auto list_response =
      server.Handle({.method = "GET",
                     .target = "/api/tasks?pageSize=10",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = {},
                     .remote_address = {}});
  ASSERT_TRUE(list_response.ok());
  EXPECT_EQ(list_response.value().status_code, 200);
  EXPECT_NE(list_response.value().body.find("task-functional-1"), std::string::npos);

  const auto cancel_response =
      server.Handle({.method = "POST",
                     .target = "/api/tasks/task-functional-1:cancel",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = "{}",
                     .remote_address = {}});
  ASSERT_TRUE(cancel_response.ok());
  EXPECT_EQ(cancel_response.value().status_code, 200);

  lf::a2a::v1::Task canceled_task;
  ASSERT_TRUE(a2a::core::JsonToMessage(cancel_response.value().body, &canceled_task).ok());
  EXPECT_EQ(canceled_task.status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

TEST(RestServerTransportFunctionalTest, ReturnsStructuredNotFoundForMalformedInput) {
  a2a::server::InMemoryTaskStore store;
  StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/api"});

  const auto response =
      server.Handle({.method = "GET",
                     .target = "/api/tasks?pageSize=abc",
                     .headers = {{"A2A-Version", "1.0"}},
                     .body = {},
                     .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 404);
  EXPECT_NE(response.value().body.find("validation_error"), std::string::npos);
}

}  // namespace
