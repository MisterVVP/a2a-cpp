#include "a2a/client/client.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

class FakeClientTransport final : public a2a::client::ClientTransport {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)options;
    lf::a2a::v1::SendMessageResponse response;
    response.mutable_message()->set_role("assistant");
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               const a2a::client::CallOptions& options) override {
    (void)options;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<a2a::client::ListTasksResponse> ListTasks(
      const a2a::client::ListTasksRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)options;
    if (list_tasks_error.has_value()) {
      return list_tasks_error.value();
    }
    return list_tasks_response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(
      const lf::a2a::v1::CancelTaskRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)options;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> SetTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request,
      const a2a::client::CallOptions& options) override {
    (void)options;
    return request;
  }

  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)options;
    lf::a2a::v1::TaskPushNotificationConfig config;
    config.set_id(request.id());
    return config;
  }

  a2a::core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)options;
    return lf::a2a::v1::ListTaskPushNotificationConfigsResponse{};
  }

  a2a::core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)options;
    return {};
  }

  a2a::core::Result<std::unique_ptr<a2a::client::StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::client::StreamObserver& observer,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)observer;
    (void)options;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<std::unique_ptr<a2a::client::StreamHandle>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, a2a::client::StreamObserver& observer,
      const a2a::client::CallOptions& options) override {
    (void)request;
    (void)observer;
    (void)options;
    return a2a::core::Error::Validation("not implemented");
  }

  a2a::core::Result<void> Shutdown() override {
    shutdown_called = true;
    return {};
  }

  bool shutdown_called = false;
  std::optional<a2a::core::Error> list_tasks_error = std::nullopt;
  a2a::client::ListTasksResponse list_tasks_response;
};

class RecordingInterceptor final : public a2a::client::ClientInterceptor {
 public:
  explicit RecordingInterceptor(std::vector<std::string>* events, std::string tag)
      : events_(events), tag_(std::move(tag)) {}

  void BeforeCall(const a2a::client::ClientCallContext& context) override {
    events_->push_back(tag_ + ":before:" + std::string(context.operation));
  }

  void AfterCall(const a2a::client::ClientCallContext& context,
                 const a2a::client::ClientCallResult& result) override {
    events_->push_back(tag_ + ":after:" + std::string(context.operation) + ":" +
                       (result.ok ? "ok" : "error"));
  }

 private:
  std::vector<std::string>* events_;
  std::string tag_;
};

TEST(A2AClientTest, ReturnsInternalErrorWhenTransportNotConfigured) {
  a2a::client::A2AClient client(nullptr);

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), a2a::core::ErrorCode::kInternal);

  class NoopObserver final : public a2a::client::StreamObserver {
   public:
    void OnEvent(const lf::a2a::v1::StreamResponse& response) override { (void)response; }
    void OnError(const a2a::core::Error& error) override { (void)error; }
    void OnCompleted() override {}
  } observer;

  lf::a2a::v1::SendMessageRequest stream_request;
  stream_request.mutable_message()->set_role("user");
  const auto stream_response = client.SendStreamingMessage(stream_request, observer);
  ASSERT_FALSE(stream_response.ok());
  EXPECT_EQ(stream_response.error().code(), a2a::core::ErrorCode::kInternal);
}

TEST(A2AClientTest, ListTasksRunsInterceptorsInExpectedOrderForSuccessAndFailure) {
  auto transport = std::make_unique<FakeClientTransport>();
  lf::a2a::v1::Task task;
  task.set_id("task-1");
  transport->list_tasks_response.tasks.push_back(task);

  a2a::client::A2AClient client(std::move(transport));
  std::vector<std::string> events;
  client.AddInterceptor(std::make_shared<RecordingInterceptor>(&events, "i1"));
  client.AddInterceptor(std::make_shared<RecordingInterceptor>(&events, "i2"));

  const auto list_result = client.ListTasks({});
  ASSERT_TRUE(list_result.ok());
  ASSERT_EQ(list_result.value().tasks.size(), 1U);

  const std::vector<std::string> expected_success = {"i1:before:ListTasks", "i2:before:ListTasks",
                                                     "i2:after:ListTasks:ok",
                                                     "i1:after:ListTasks:ok"};
  EXPECT_EQ(events, expected_success);
}

TEST(A2AClientTest, ListTasksPropagatesErrorAndInterceptorResult) {
  auto transport = std::make_unique<FakeClientTransport>();
  transport->list_tasks_error = a2a::core::Error::RemoteProtocol("upstream");
  a2a::client::A2AClient client(std::move(transport));

  std::vector<std::string> events;
  client.AddInterceptor(std::make_shared<RecordingInterceptor>(&events, "i1"));

  const auto result = client.ListTasks({});
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
  const std::vector<std::string> expected = {"i1:before:ListTasks", "i1:after:ListTasks:error"};
  EXPECT_EQ(events, expected);
}

TEST(A2AClientTest, DestroyShutsDownTransportAndClearsClient) {
  auto transport = std::make_unique<FakeClientTransport>();
  auto* transport_ptr = transport.get();
  a2a::client::A2AClient client(std::move(transport));

  const auto destroy_result = client.Destroy();
  ASSERT_TRUE(destroy_result.ok());
  EXPECT_TRUE(transport_ptr->shutdown_called);

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("task-1");
  const auto get_task = client.GetTask(request);
  ASSERT_FALSE(get_task.ok());
  EXPECT_EQ(get_task.error().code(), a2a::core::ErrorCode::kInternal);
}

}  // namespace
