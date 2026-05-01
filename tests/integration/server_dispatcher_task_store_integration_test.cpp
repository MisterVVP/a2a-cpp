#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "a2a/server/server.h"

namespace {

class StoreBackedExecutor final : public a2a::server::AgentExecutor {
 public:
  explicit StoreBackedExecutor(a2a::server::TaskStore* store) : store_(store) {}

  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    if (request.message().task_id().empty()) {
      return a2a::core::Error::Validation("message.task_id is required");
    }

    lf::a2a::v1::Task task;
    task.set_id(request.message().task_id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    const auto create_result = store_->CreateOrUpdate(task);
    if (!create_result.ok()) {
      return create_result.error();
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
    return a2a::core::Error::Validation("streaming is not enabled");
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

class TrackingServerInterceptor final : public a2a::server::ServerInterceptor {
 public:
  explicit TrackingServerInterceptor(std::vector<std::string>* events) : events_(events) {}

  a2a::core::Result<void> BeforeDispatch(const a2a::server::DispatchRequest& request,
                                         a2a::server::RequestContext& context) override {
    events_->push_back("before:" + std::to_string(static_cast<int>(request.operation)));
    context.client_headers["x-intercepted"] = "true";
    return {};
  }

  void AfterDispatch(const a2a::server::DispatchRequest& request,
                     a2a::server::RequestContext& context,
                     const a2a::core::Result<a2a::server::DispatchResponse>& result) override {
    (void)context;
    events_->push_back("after:" + std::to_string(static_cast<int>(request.operation)) + ":" +
                       (result.ok() ? "ok" : "error"));
  }

 private:
  std::vector<std::string>* events_;
};

TEST(ServerDispatcherTaskStoreIntegrationTest, ExecutesTaskLifecycleThroughDispatcher) {
  a2a::server::InMemoryTaskStore store;
  StoreBackedExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role("user");
  send_request.mutable_message()->set_task_id("integration-task-1");

  const auto send_result = dispatcher.Dispatch(
      {.operation = a2a::server::DispatcherOperation::kSendMessage, .payload = send_request},
      context);
  ASSERT_TRUE(send_result.ok());

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("integration-task-1");
  const auto get_result = dispatcher.Dispatch(
      {.operation = a2a::server::DispatcherOperation::kGetTask, .payload = get_request}, context);
  ASSERT_TRUE(get_result.ok());
  const auto& get_task = std::get<lf::a2a::v1::Task>(get_result.value().payload());
  EXPECT_EQ(get_task.status().state(), lf::a2a::v1::TASK_STATE_WORKING);

  const auto list_result =
      dispatcher.Dispatch({.operation = a2a::server::DispatcherOperation::kListTasks,
                           .payload = a2a::server::ListTasksRequest{}},
                          context);
  ASSERT_TRUE(list_result.ok());
  const auto& list_payload =
      std::get<a2a::server::ListTasksResponse>(list_result.value().payload());
  ASSERT_EQ(list_payload.tasks.size(), 1U);
  EXPECT_EQ(list_payload.tasks.front().id(), "integration-task-1");

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id("integration-task-1");
  const auto cancel_result = dispatcher.Dispatch(
      {.operation = a2a::server::DispatcherOperation::kCancelTask, .payload = cancel_request},
      context);
  ASSERT_TRUE(cancel_result.ok());
  const auto& canceled_task = std::get<lf::a2a::v1::Task>(cancel_result.value().payload());
  EXPECT_EQ(canceled_task.status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

TEST(ServerDispatcherTaskStoreIntegrationTest, InterceptorsObserveRequestLifecycle) {
  a2a::server::InMemoryTaskStore store;
  StoreBackedExecutor executor(&store);
  std::vector<std::string> events;
  a2a::server::Dispatcher dispatcher(&executor,
                                     {std::make_shared<TrackingServerInterceptor>(&events)});
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role("user");
  send_request.mutable_message()->set_task_id("integration-task-2");

  const auto send_result = dispatcher.Dispatch(
      {.operation = a2a::server::DispatcherOperation::kSendMessage, .payload = send_request},
      context);
  ASSERT_TRUE(send_result.ok());
  EXPECT_EQ(context.client_headers["x-intercepted"], "true");

  const std::vector<std::string> expected = {"before:0", "after:0:ok"};
  EXPECT_EQ(events, expected);
}

}  // namespace
