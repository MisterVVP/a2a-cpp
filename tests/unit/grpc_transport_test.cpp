#include "a2a/client/grpc_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/client/auth.h"

namespace {

class FakeStreamReader final : public a2a::client::GrpcTransport::StreamReader {
 public:
  explicit FakeStreamReader(std::vector<lf::a2a::v1::StreamResponse> events,
                            grpc::Status finish_status = grpc::Status::OK)
      : events_(std::move(events)), finish_status_(std::move(finish_status)) {}

  bool Read(lf::a2a::v1::StreamResponse* response) override {
    if (index_ >= events_.size()) {
      return false;
    }
    *response = events_[index_++];
    return true;
  }

  grpc::Status Finish() override { return finish_status_; }

 private:
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::size_t index_ = 0;
  grpc::Status finish_status_;
};

class FakeRpcClient final : public a2a::client::GrpcTransport::RpcClient {
 public:
  grpc::Status SendMessage(grpc::ClientContext* context,
                           const lf::a2a::v1::SendMessageRequest& request,
                           lf::a2a::v1::SendMessageResponse* response) override {
    (void)context;
    last_task_id = request.message().task_id();
    response->mutable_task()->set_id(request.message().task_id());
    return send_status;
  }

  std::unique_ptr<a2a::client::GrpcTransport::StreamReader> SendStreamingMessage(
      grpc::ClientContext* context, const lf::a2a::v1::SendMessageRequest& request) override {
    (void)context;
    last_task_id = request.message().task_id();
    return std::move(stream_reader);
  }

  grpc::Status GetTask(grpc::ClientContext* context, const lf::a2a::v1::GetTaskRequest& request,
                       lf::a2a::v1::Task* response) override {
    (void)context;
    response->set_id(request.id());
    response->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    return task_status;
  }

  grpc::Status CancelTask(grpc::ClientContext* context,
                          const lf::a2a::v1::CancelTaskRequest& request,
                          lf::a2a::v1::Task* response) override {
    (void)context;
    response->set_id(request.id());
    response->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return cancel_status;
  }

  grpc::Status SetTaskPushNotificationConfig(
      grpc::ClientContext* context, const lf::a2a::v1::TaskPushNotificationConfig& request,
      lf::a2a::v1::TaskPushNotificationConfig* response) override {
    (void)context;
    *response = request;
    return grpc::Status::OK;
  }

  grpc::Status GetTaskPushNotificationConfig(
      grpc::ClientContext* context, const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      lf::a2a::v1::TaskPushNotificationConfig* response) override {
    (void)context;
    response->set_id(request.id());
    return grpc::Status::OK;
  }

  grpc::Status ListTaskPushNotificationConfigs(
      grpc::ClientContext* context,
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) override {
    (void)context;
    response->set_next_page_token(request.page_token());
    return grpc::Status::OK;
  }

  grpc::Status DeleteTaskPushNotificationConfig(
      grpc::ClientContext* context,
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      google::protobuf::Empty* response) override {
    (void)context;
    (void)request;
    (void)response;
    return grpc::Status::OK;
  }

  grpc::Status send_status = grpc::Status::OK;
  grpc::Status task_status = grpc::Status::OK;
  grpc::Status cancel_status = grpc::Status::OK;
  std::unique_ptr<a2a::client::GrpcTransport::StreamReader> stream_reader;
  std::string last_task_id;
};

class RecordingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    events.push_back(response);
  }

  void OnError(const a2a::core::Error& error) override { last_error = error; }

  void OnCompleted() override { completed = true; }

  std::vector<lf::a2a::v1::StreamResponse> events;
  std::optional<a2a::core::Error> last_error;
  bool completed = false;
};

TEST(GrpcTransportTest, GetTaskValidatesRequestId) {
  auto rpc = std::make_unique<FakeRpcClient>();
  a2a::client::GrpcTransport transport(
      {.transport = a2a::client::PreferredTransport::kGrpc,
       .url = "localhost:50051",
       .security_requirements = {},
       .security_schemes = {}},
      std::move(rpc));

  lf::a2a::v1::GetTaskRequest request;
  const auto result = transport.GetTask(request, {});

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(GrpcTransportTest, SendStreamingMessageDeliversEventsAndCompletion) {
  auto rpc = std::make_unique<FakeRpcClient>();
  lf::a2a::v1::StreamResponse event;
  event.mutable_task()->set_id("stream-task");
  rpc->stream_reader = std::make_unique<FakeStreamReader>(std::vector{event});

  a2a::client::GrpcTransport transport(
      {.transport = a2a::client::PreferredTransport::kGrpc,
       .url = "localhost:50051",
       .security_requirements = {},
       .security_schemes = {}},
      std::move(rpc));

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id("stream-task");

  RecordingObserver observer;
  auto stream = transport.SendStreamingMessage(request, observer, {});
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  while (stream.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(observer.events.size(), 1U);
  EXPECT_EQ(observer.events.front().task().id(), "stream-task");
  EXPECT_TRUE(observer.completed);
  EXPECT_FALSE(observer.last_error.has_value());
}

TEST(GrpcTransportTest, SubscribeTaskEmitsSingleTaskEvent) {
  auto rpc = std::make_unique<FakeRpcClient>();
  a2a::client::GrpcTransport transport(
      {.transport = a2a::client::PreferredTransport::kGrpc,
       .url = "localhost:50051",
       .security_requirements = {},
       .security_schemes = {}},
      std::move(rpc));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("sub-task");

  RecordingObserver observer;
  auto stream = transport.SubscribeTask(request, observer, {});
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  while (stream.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(observer.events.size(), 1U);
  EXPECT_EQ(observer.events.front().task().id(), "sub-task");
  EXPECT_TRUE(observer.completed);
}

}  // namespace
