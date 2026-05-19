#include "a2a/client/grpc_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
  grpc::Status SendMessage(grpc::ClientContext* context, const lf::a2a::v1::SendMessageRequest& request,
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

  grpc::Status CancelTask(grpc::ClientContext* context, const lf::a2a::v1::CancelTaskRequest& request,
                          lf::a2a::v1::Task* response) override {
    (void)context;
    response->set_id(request.id());
    response->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return cancel_status;
  }

  grpc::Status CreateTaskPushNotificationConfig(grpc::ClientContext* context,
                                                const lf::a2a::v1::TaskPushNotificationConfig& request,
                                                lf::a2a::v1::TaskPushNotificationConfig* response) override {
    (void)context;
    *response = request;
    return create_config_status;
  }

  grpc::Status GetTaskPushNotificationConfig(grpc::ClientContext* context,
                                             const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
                                             lf::a2a::v1::TaskPushNotificationConfig* response) override {
    (void)context;
    response->set_id(request.id());
    return get_config_status;
  }

  grpc::Status ListTaskPushNotificationConfigs(
      grpc::ClientContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) override {
    (void)context;
    response->set_next_page_token(request.page_token());
    return list_configs_status;
  }

  grpc::Status DeleteTaskPushNotificationConfig(grpc::ClientContext* context,
                                                const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
                                                google::protobuf::Empty* response) override {
    (void)context;
    (void)request;
    (void)response;
    return delete_config_status;
  }

  grpc::Status send_status = grpc::Status::OK;
  grpc::Status task_status = grpc::Status::OK;
  grpc::Status cancel_status = grpc::Status::OK;
  grpc::Status create_config_status = grpc::Status::OK;
  grpc::Status get_config_status = grpc::Status::OK;
  grpc::Status list_configs_status = grpc::Status::OK;
  grpc::Status delete_config_status = grpc::Status::OK;
  std::unique_ptr<a2a::client::GrpcTransport::StreamReader> stream_reader;
  std::string last_task_id;
};

class RecordingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override { events.push_back(response); }

  void OnError(const a2a::core::Error& error) override { last_error = error; }

  void OnCompleted() override { completed = true; }

  std::vector<lf::a2a::v1::StreamResponse> events;
  std::optional<a2a::core::Error> last_error;
  bool completed = false;
};

TEST(GrpcTransportTest, GetTaskValidatesRequestId) {
  auto rpc = std::make_unique<FakeRpcClient>();
  a2a::client::GrpcTransport transport({.transport = a2a::client::PreferredTransport::kGrpc,
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

  a2a::client::GrpcTransport transport({.transport = a2a::client::PreferredTransport::kGrpc,
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
  a2a::client::GrpcTransport transport({.transport = a2a::client::PreferredTransport::kGrpc,
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

TEST(GrpcTransportTest, PushNotificationConfigCrudAndListCoverSuccessAndValidation) {
  constexpr std::string_view kTaskId = "task-123";
  auto rpc = std::make_unique<FakeRpcClient>();
  auto* rpc_ptr = rpc.get();
  a2a::client::GrpcTransport transport({.transport = a2a::client::PreferredTransport::kGrpc,
                                        .url = "localhost:50051",
                                        .security_requirements = {},
                                        .security_schemes = {}},
                                       std::move(rpc));

  lf::a2a::v1::TaskPushNotificationConfig create_request;
  create_request.set_id(std::string(kTaskId));
  const auto create_result = transport.CreateTaskPushNotificationConfig(create_request, {});
  ASSERT_TRUE(create_result.ok()) << create_result.error().message();
  EXPECT_EQ(create_result.value().id(), kTaskId);

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_id(std::string(kTaskId));
  const auto get_result = transport.GetTaskPushNotificationConfig(get_request, {});
  ASSERT_TRUE(get_result.ok()) << get_result.error().message();
  EXPECT_EQ(get_result.value().id(), kTaskId);

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  list_request.set_page_token("next");
  const auto list_result = transport.ListTaskPushNotificationConfigs(list_request, {});
  ASSERT_TRUE(list_result.ok()) << list_result.error().message();
  EXPECT_EQ(list_result.value().next_page_token(), "next");

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_id(std::string(kTaskId));
  const auto delete_result = transport.DeleteTaskPushNotificationConfig(delete_request, {});
  ASSERT_TRUE(delete_result.ok()) << delete_result.error().message();

  lf::a2a::v1::GetTaskPushNotificationConfigRequest missing_get_id;
  const auto get_validation_error = transport.GetTaskPushNotificationConfig(missing_get_id, {});
  ASSERT_FALSE(get_validation_error.ok());
  EXPECT_EQ(get_validation_error.error().code(), a2a::core::ErrorCode::kValidation);

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest missing_delete_id;
  const auto delete_validation_error = transport.DeleteTaskPushNotificationConfig(missing_delete_id, {});
  ASSERT_FALSE(delete_validation_error.ok());
  EXPECT_EQ(delete_validation_error.error().code(), a2a::core::ErrorCode::kValidation);

  (void)rpc_ptr;
}

TEST(GrpcTransportTest, MapsGrpcStatusesToRemoteProtocolError) {
  constexpr std::string_view kMessage = "permission denied";
  auto rpc = std::make_unique<FakeRpcClient>();
  rpc->send_status = grpc::Status(grpc::StatusCode::PERMISSION_DENIED, std::string(kMessage));
  rpc->task_status = grpc::Status(grpc::StatusCode::NOT_FOUND, "task missing");
  rpc->cancel_status = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "cannot cancel");
  rpc->create_config_status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "temporarily unavailable");
  rpc->get_config_status = grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "unauthenticated");
  rpc->list_configs_status = grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "deadline");
  rpc->delete_config_status = grpc::Status(grpc::StatusCode::ABORTED, "aborted");
  auto* rpc_ptr = rpc.get();

  a2a::client::GrpcTransport transport({.transport = a2a::client::PreferredTransport::kGrpc,
                                        .url = "localhost:50051",
                                        .security_requirements = {},
                                        .security_schemes = {}},
                                       std::move(rpc));

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_task_id("task");
  const auto send_result = transport.SendMessage(send_request, {});
  ASSERT_FALSE(send_result.ok());
  EXPECT_EQ(send_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::GetTaskRequest get_task_request;
  get_task_request.set_id("task");
  const auto get_task_result = transport.GetTask(get_task_request, {});
  ASSERT_FALSE(get_task_result.ok());
  EXPECT_EQ(get_task_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::CancelTaskRequest cancel_task_request;
  cancel_task_request.set_id("task");
  const auto cancel_result = transport.CancelTask(cancel_task_request, {});
  ASSERT_FALSE(cancel_result.ok());
  EXPECT_EQ(cancel_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::TaskPushNotificationConfig create_request;
  const auto create_result = transport.CreateTaskPushNotificationConfig(create_request, {});
  ASSERT_FALSE(create_result.ok());
  EXPECT_EQ(create_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_id("task");
  const auto get_result = transport.GetTaskPushNotificationConfig(get_request, {});
  ASSERT_FALSE(get_result.ok());
  EXPECT_EQ(get_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  const auto list_result = transport.ListTaskPushNotificationConfigs(list_request, {});
  ASSERT_FALSE(list_result.ok());
  EXPECT_EQ(list_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_id("task");
  const auto delete_result = transport.DeleteTaskPushNotificationConfig(delete_request, {});
  ASSERT_FALSE(delete_result.ok());
  EXPECT_EQ(delete_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
  (void)rpc_ptr;
}

}  // namespace
