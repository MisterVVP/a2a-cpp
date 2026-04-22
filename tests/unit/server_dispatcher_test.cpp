#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/server/server.h"

namespace {

class SingleEventSession final : public a2a::server::ServerStreamSession {
 public:
  explicit SingleEventSession(lf::a2a::v1::StreamResponse event) : event_(std::move(event)) {}

  a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (consumed_) {
      return std::optional<lf::a2a::v1::StreamResponse>{};
    }
    consumed_ = true;
    return std::optional<lf::a2a::v1::StreamResponse>{event_};
  }

 private:
  lf::a2a::v1::StreamResponse event_;
  bool consumed_ = false;
};

class FakeExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    last_request_id = context.request_id.value_or("missing");
    if (request.message().role().empty()) {
      return a2a::core::Error::Validation("message role is required");
    }
    lf::a2a::v1::SendMessageResponse response;
    response.mutable_message()->set_role("assistant");
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    lf::a2a::v1::StreamResponse event;
    event.mutable_message()->set_role("assistant");
    return std::unique_ptr<a2a::server::ServerStreamSession>(
        std::make_unique<SingleEventSession>(std::move(event)));
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    if (request.id() == "missing") {
      return a2a::core::Error::RemoteProtocol("task not found");
    }
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(
      const a2a::server::ListTasksRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    a2a::server::ListTasksResponse response;
    lf::a2a::v1::Task task;
    task.set_id("task-1");
    response.tasks.push_back(task);
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return task;
  }

  std::string last_request_id;
};

TEST(ServerDispatcherTest, DispatchesAllSupportedOperations) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RequestContext context;
  context.request_id = "req-123";

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role("user");
  const a2a::server::DispatchRequest send_dispatch{
      .operation = a2a::server::DispatcherOperation::kSendMessage, .payload = send_request};
  const auto send_result = dispatcher.Dispatch(send_dispatch, context);
  ASSERT_TRUE(send_result.ok());
  EXPECT_TRUE(
      std::holds_alternative<lf::a2a::v1::SendMessageResponse>(send_result.value().payload()));
  EXPECT_EQ(executor.last_request_id, "req-123");

  const a2a::server::DispatchRequest stream_dispatch{
      .operation = a2a::server::DispatcherOperation::kSendStreamingMessage,
      .payload = send_request};
  const auto stream_result = dispatcher.Dispatch(stream_dispatch, context);
  ASSERT_TRUE(stream_result.ok());
  ASSERT_TRUE(std::holds_alternative<std::unique_ptr<a2a::server::ServerStreamSession>>(
      stream_result.value().payload()));

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("task-7");
  const a2a::server::DispatchRequest get_dispatch{
      .operation = a2a::server::DispatcherOperation::kGetTask, .payload = get_request};
  const auto get_result = dispatcher.Dispatch(get_dispatch, context);
  ASSERT_TRUE(get_result.ok());
  ASSERT_TRUE(std::holds_alternative<lf::a2a::v1::Task>(get_result.value().payload()));

  const a2a::server::DispatchRequest list_dispatch{
      .operation = a2a::server::DispatcherOperation::kListTasks,
      .payload = a2a::server::ListTasksRequest{.page_size = 10, .page_token = ""}};
  const auto list_result = dispatcher.Dispatch(list_dispatch, context);
  ASSERT_TRUE(list_result.ok());
  ASSERT_TRUE(
      std::holds_alternative<a2a::server::ListTasksResponse>(list_result.value().payload()));

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id("task-7");
  const a2a::server::DispatchRequest cancel_dispatch{
      .operation = a2a::server::DispatcherOperation::kCancelTask, .payload = cancel_request};
  const auto cancel_result = dispatcher.Dispatch(cancel_dispatch, context);
  ASSERT_TRUE(cancel_result.ok());
  ASSERT_TRUE(std::holds_alternative<lf::a2a::v1::Task>(cancel_result.value().payload()));
}

TEST(ServerDispatcherTest, ReturnsValidationErrorForPayloadMismatch) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RequestContext context;

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("task-7");

  const a2a::server::DispatchRequest dispatch{
      .operation = a2a::server::DispatcherOperation::kSendMessage, .payload = get_request};
  const auto response = dispatcher.Dispatch(dispatch, context);

  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(ServerDispatcherTest, PropagatesExecutorErrors) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RequestContext context;

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("missing");
  const a2a::server::DispatchRequest dispatch{
      .operation = a2a::server::DispatcherOperation::kGetTask, .payload = request};

  const auto response = dispatcher.Dispatch(dispatch, context);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
}

TEST(ServerDispatcherTest, ReturnsInternalErrorWithoutExecutor) {
  a2a::server::Dispatcher dispatcher(nullptr);
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role("user");

  const a2a::server::DispatchRequest dispatch{
      .operation = a2a::server::DispatcherOperation::kSendMessage, .payload = request};

  const auto response = dispatcher.Dispatch(dispatch, context);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), a2a::core::ErrorCode::kInternal);
}

}  // namespace
