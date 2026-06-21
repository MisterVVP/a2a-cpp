// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/grpc_transport.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/client/auth.h"

namespace {

const std::string kGrpcEndpoint = "localhost:50051";
const std::string kAuthHeaderName = "X-Test-API-Key";
const std::string kStreamFailureMessage = "stream unavailable";
const std::string kStreamCancelledMessage = "stream cancelled";
constexpr int kStreamPollIntervalMs = 1;
constexpr int kCancellationWaitAttempts = 1000;

struct BlockingStreamState final {
  void Cancel() {
    {
      std::lock_guard lock(mutex);
      cancelled = true;
    }
    ready.notify_all();
  }

  std::mutex mutex;
  std::condition_variable ready;
  bool cancelled = false;
};

class BlockingStreamReader final : public a2a::client::GrpcTransport::StreamReader {
 public:
  explicit BlockingStreamReader(std::shared_ptr<BlockingStreamState> state) : state_(std::move(state)) {}

  bool Read(lf::a2a::v1::StreamResponse* response) override {
    (void)response;
    std::unique_lock lock(state_->mutex);
    state_->ready.wait(lock, [this] { return state_->cancelled; });
    return false;
  }

  grpc::Status Finish() override { return {grpc::StatusCode::CANCELLED, kStreamCancelledMessage}; }

 private:
  std::shared_ptr<BlockingStreamState> state_;
};

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

  std::unique_ptr<a2a::client::GrpcTransport::StreamReader> SubscribeToTask(
      grpc::ClientContext* context, const lf::a2a::v1::SubscribeToTaskRequest& request) override {
    (void)context;
    last_task_id = request.id();
    return std::move(stream_reader);
  }

  void CancelStream(grpc::ClientContext* context) override {
    a2a::client::GrpcTransport::RpcClient::CancelStream(context);
    if (blocking_stream_state != nullptr) {
      blocking_stream_state->Cancel();
    }
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
  std::shared_ptr<BlockingStreamState> blocking_stream_state;
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

a2a::client::ResolvedInterface MakeResolvedInterface(
    a2a::client::PreferredTransport transport = a2a::client::PreferredTransport::kGrpc,
    std::string url = kGrpcEndpoint) {
  return {.transport = transport, .url = std::move(url), .security_requirements = {}, .security_schemes = {}};
}

lf::a2a::v1::SendMessageRequest MakeSendMessageRequest(std::string_view task_id) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id(std::string(task_id));
  return request;
}

lf::a2a::v1::StreamResponse MakeTaskStreamEvent(std::string_view task_id) {
  lf::a2a::v1::StreamResponse event;
  event.mutable_task()->set_id(std::string(task_id));
  return event;
}

void WaitForStream(const a2a::client::StreamHandle& stream) {
  while (stream.IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kStreamPollIntervalMs));
  }
}

void ExpectObserverErrorCode(const RecordingObserver& observer, a2a::core::ErrorCode expected_code) {
  ASSERT_TRUE(observer.last_error.has_value());
  EXPECT_EQ(observer.last_error.value_or(a2a::core::Error::Internal("missing observer error")).code(), expected_code);
}

TEST(GrpcTransportTest, GetTaskValidatesRequestId) {
  auto rpc = std::make_unique<FakeRpcClient>();
  a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

  lf::a2a::v1::GetTaskRequest request;
  const auto result = transport.GetTask(request, {});

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(GrpcTransportTest, SendMessageValidatesBuildContextFailures) {
  constexpr std::string_view kTaskId = "context-task";

  {
    auto rpc = std::make_unique<FakeRpcClient>();
    a2a::client::GrpcTransport transport(MakeResolvedInterface(a2a::client::PreferredTransport::kRest), std::move(rpc));
    const auto result = transport.SendMessage(MakeSendMessageRequest(kTaskId), {});

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  }

  {
    auto rpc = std::make_unique<FakeRpcClient>();
    a2a::client::GrpcTransport transport(MakeResolvedInterface(a2a::client::PreferredTransport::kGrpc, {}),
                                         std::move(rpc));
    const auto result = transport.SendMessage(MakeSendMessageRequest(kTaskId), {});

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  }

  {
    a2a::client::GrpcTransport transport(MakeResolvedInterface(),
                                         std::unique_ptr<a2a::client::GrpcTransport::RpcClient>{});
    const auto result = transport.SendMessage(MakeSendMessageRequest(kTaskId), {});

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kInternal);
  }

  {
    auto rpc = std::make_unique<FakeRpcClient>();
    a2a::client::CallOptions options;
    options.credential_provider =
        std::make_shared<a2a::client::ApiKeyCredentialProvider>(std::string{}, kAuthHeaderName);
    a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));
    const auto result = transport.SendMessage(MakeSendMessageRequest(kTaskId), options);

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  }
}

TEST(GrpcTransportTest, SendStreamingMessageDeliversEventsAndCompletion) {
  constexpr std::string_view kTaskId = "stream-task";
  auto rpc = std::make_unique<FakeRpcClient>();
  rpc->stream_reader = std::make_unique<FakeStreamReader>(std::vector{MakeTaskStreamEvent(kTaskId)});

  a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

  RecordingObserver observer;
  auto stream = transport.SendStreamingMessage(MakeSendMessageRequest(kTaskId), observer, {});
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  WaitForStream(*stream.value());

  ASSERT_EQ(observer.events.size(), 1U);
  EXPECT_EQ(observer.events.front().task().id(), std::string(kTaskId));
  EXPECT_TRUE(observer.completed);
  EXPECT_FALSE(observer.last_error.has_value());
}

TEST(GrpcTransportTest, SendStreamingMessageReportsReaderAndFinishErrors) {
  constexpr std::string_view kTaskId = "stream-error-task";

  {
    auto rpc = std::make_unique<FakeRpcClient>();
    a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

    RecordingObserver observer;
    auto stream = transport.SendStreamingMessage(MakeSendMessageRequest(kTaskId), observer, {});
    ASSERT_TRUE(stream.ok()) << stream.error().message();

    WaitForStream(*stream.value());

    EXPECT_TRUE(observer.events.empty());
    EXPECT_FALSE(observer.completed);
    ExpectObserverErrorCode(observer, a2a::core::ErrorCode::kInternal);
  }

  {
    auto rpc = std::make_unique<FakeRpcClient>();
    rpc->stream_reader = std::make_unique<FakeStreamReader>(
        std::vector<lf::a2a::v1::StreamResponse>{}, grpc::Status(grpc::StatusCode::UNAVAILABLE, kStreamFailureMessage));
    a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

    RecordingObserver observer;
    auto stream = transport.SendStreamingMessage(MakeSendMessageRequest(kTaskId), observer, {});
    ASSERT_TRUE(stream.ok()) << stream.error().message();

    WaitForStream(*stream.value());

    EXPECT_TRUE(observer.events.empty());
    EXPECT_FALSE(observer.completed);
    ExpectObserverErrorCode(observer, a2a::core::ErrorCode::kRemoteProtocol);
  }
}

TEST(GrpcTransportTest, SubscribeTaskEmitsSingleTaskEvent) {
  constexpr std::string_view kTaskId = "sub-task";
  auto rpc = std::make_unique<FakeRpcClient>();
  rpc->stream_reader = std::make_unique<FakeStreamReader>(std::vector{MakeTaskStreamEvent(kTaskId)});

  a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kTaskId));

  RecordingObserver observer;
  auto stream = transport.SubscribeTask(request, observer, {});
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  WaitForStream(*stream.value());

  ASSERT_EQ(observer.events.size(), 1U);
  EXPECT_EQ(observer.events.front().task().id(), std::string(kTaskId));
  EXPECT_TRUE(observer.completed);
  EXPECT_FALSE(observer.last_error.has_value());
}

TEST(GrpcTransportTest, SubscribeTaskCancellationInterruptsBlockedRead) {
  constexpr std::string_view kTaskId = "blocked-subscription-task";
  auto blocking_state = std::make_shared<BlockingStreamState>();
  auto rpc = std::make_unique<FakeRpcClient>();
  rpc->blocking_stream_state = blocking_state;
  rpc->stream_reader = std::make_unique<BlockingStreamReader>(blocking_state);
  a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kTaskId));
  RecordingObserver observer;
  auto stream = transport.SubscribeTask(request, observer, {});
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  std::atomic_bool cancellation_completed = false;
  std::thread canceller([&stream, &cancellation_completed] {
    stream.value()->Cancel();
    cancellation_completed.store(true);
  });

  for (int attempt = 0; attempt < kCancellationWaitAttempts && !cancellation_completed.load(); ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kStreamPollIntervalMs));
  }
  if (!cancellation_completed.load()) {
    blocking_state->Cancel();
  }
  canceller.join();

  EXPECT_TRUE(cancellation_completed.load());
  EXPECT_FALSE(stream.value()->IsActive());
  EXPECT_FALSE(observer.completed);
  EXPECT_FALSE(observer.last_error.has_value());
}

TEST(GrpcTransportTest, SubscribeTaskValidatesInputAndReportsStreamErrors) {
  constexpr std::string_view kTaskId = "sub-error-task";

  {
    auto rpc = std::make_unique<FakeRpcClient>();
    a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

    lf::a2a::v1::GetTaskRequest request;
    RecordingObserver observer;
    const auto stream = transport.SubscribeTask(request, observer, {});

    ASSERT_FALSE(stream.ok());
    EXPECT_EQ(stream.error().code(), a2a::core::ErrorCode::kValidation);
  }

  {
    auto rpc = std::make_unique<FakeRpcClient>();
    a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

    lf::a2a::v1::GetTaskRequest request;
    request.set_id(std::string(kTaskId));
    RecordingObserver observer;
    auto stream = transport.SubscribeTask(request, observer, {});
    ASSERT_TRUE(stream.ok()) << stream.error().message();

    WaitForStream(*stream.value());

    EXPECT_TRUE(observer.events.empty());
    EXPECT_FALSE(observer.completed);
    ExpectObserverErrorCode(observer, a2a::core::ErrorCode::kInternal);
  }

  {
    auto rpc = std::make_unique<FakeRpcClient>();
    rpc->stream_reader = std::make_unique<FakeStreamReader>(
        std::vector<lf::a2a::v1::StreamResponse>{}, grpc::Status(grpc::StatusCode::UNAVAILABLE, kStreamFailureMessage));
    a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

    lf::a2a::v1::GetTaskRequest request;
    request.set_id(std::string(kTaskId));
    RecordingObserver observer;
    auto stream = transport.SubscribeTask(request, observer, {});
    ASSERT_TRUE(stream.ok()) << stream.error().message();

    WaitForStream(*stream.value());

    EXPECT_TRUE(observer.events.empty());
    EXPECT_FALSE(observer.completed);
    ExpectObserverErrorCode(observer, a2a::core::ErrorCode::kRemoteProtocol);
  }
}

TEST(GrpcTransportTest, PushNotificationConfigCrudAndListCoverSuccessAndValidation) {
  constexpr std::string_view kTaskId = "task-123";
  auto rpc = std::make_unique<FakeRpcClient>();
  auto* rpc_ptr = rpc.get();
  a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

  lf::a2a::v1::TaskPushNotificationConfig create_request;
  create_request.set_id(std::string(kTaskId));
  const auto create_result = transport.CreateTaskPushNotificationConfig(create_request, {});
  ASSERT_TRUE(create_result.ok()) << create_result.error().message();
  EXPECT_EQ(create_result.value().id(), std::string(kTaskId));

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_id(std::string(kTaskId));
  const auto get_result = transport.GetTaskPushNotificationConfig(get_request, {});
  ASSERT_TRUE(get_result.ok()) << get_result.error().message();
  EXPECT_EQ(get_result.value().id(), std::string(kTaskId));

  constexpr std::string_view kNextPageToken = "next";
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  list_request.set_page_token(std::string(kNextPageToken));
  const auto list_result = transport.ListTaskPushNotificationConfigs(list_request, {});
  ASSERT_TRUE(list_result.ok()) << list_result.error().message();
  EXPECT_EQ(list_result.value().next_page_token(), std::string(kNextPageToken));

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
  constexpr std::string_view kTaskId = "task";
  auto rpc = std::make_unique<FakeRpcClient>();
  rpc->send_status = grpc::Status(grpc::StatusCode::PERMISSION_DENIED, std::string(kMessage));
  rpc->task_status = grpc::Status(grpc::StatusCode::NOT_FOUND, "task missing");
  rpc->cancel_status = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "cannot cancel");
  rpc->create_config_status = grpc::Status(grpc::StatusCode::UNAVAILABLE, "temporarily unavailable");
  rpc->get_config_status = grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "unauthenticated");
  rpc->list_configs_status = grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "deadline");
  rpc->delete_config_status = grpc::Status(grpc::StatusCode::ABORTED, "aborted");
  auto* rpc_ptr = rpc.get();

  a2a::client::GrpcTransport transport(MakeResolvedInterface(), std::move(rpc));

  const auto send_result = transport.SendMessage(MakeSendMessageRequest(kTaskId), {});
  ASSERT_FALSE(send_result.ok());
  EXPECT_EQ(send_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::GetTaskRequest get_task_request;
  get_task_request.set_id(std::string(kTaskId));
  const auto get_task_result = transport.GetTask(get_task_request, {});
  ASSERT_FALSE(get_task_result.ok());
  EXPECT_EQ(get_task_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::CancelTaskRequest cancel_task_request;
  cancel_task_request.set_id(std::string(kTaskId));
  const auto cancel_result = transport.CancelTask(cancel_task_request, {});
  ASSERT_FALSE(cancel_result.ok());
  EXPECT_EQ(cancel_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::TaskPushNotificationConfig create_request;
  const auto create_result = transport.CreateTaskPushNotificationConfig(create_request, {});
  ASSERT_FALSE(create_result.ok());
  EXPECT_EQ(create_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_id(std::string(kTaskId));
  const auto get_result = transport.GetTaskPushNotificationConfig(get_request, {});
  ASSERT_FALSE(get_result.ok());
  EXPECT_EQ(get_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  const auto list_result = transport.ListTaskPushNotificationConfigs(list_request, {});
  ASSERT_FALSE(list_result.ok());
  EXPECT_EQ(list_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_id(std::string(kTaskId));
  const auto delete_result = transport.DeleteTaskPushNotificationConfig(delete_request, {});
  ASSERT_FALSE(delete_result.ok());
  EXPECT_EQ(delete_result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
  (void)rpc_ptr;
}

}  // namespace
