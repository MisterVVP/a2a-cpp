// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/grpc_transport.h"
#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/version.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/request_context.h"
#include "a2a/server/server_stream_session.h"
#include "a2a/server/task_subscription_service.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"

namespace {

class StreamSession final : public a2a::server::ServerStreamSession {
 public:
  explicit StreamSession(std::vector<lf::a2a::v1::StreamResponse> events) : events_(std::move(events)) {}

  a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (index_ >= events_.size()) {
      return std::optional<lf::a2a::v1::StreamResponse>{};
    }
    return std::optional<lf::a2a::v1::StreamResponse>(events_[index_++]);
  }

 private:
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::size_t index_ = 0;
};

constexpr std::string_view kClientCancelledSubscribeTaskId = "grpc-client-cancel-subscribe";
constexpr auto kClientCancellationTimeout = std::chrono::seconds(2);

class ControlledSubscriptionRecorder final {
 public:
  void RecordCancel() {
    cancel_count_.fetch_add(1);
    {
      std::lock_guard lock(mutex_);
      cancelled_ = true;
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool WaitForCancel() const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kClientCancellationTimeout, [this] { return cancelled_; });
  }

  [[nodiscard]] int CancelCount() const noexcept { return cancel_count_.load(); }

 private:
  std::atomic_int cancel_count_ = 0;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  bool cancelled_ = false;
};

class ControlledSubscriptionSession final : public a2a::server::ServerStreamSession {
 public:
  ControlledSubscriptionSession(lf::a2a::v1::Task task, std::shared_ptr<ControlledSubscriptionRecorder> recorder)
      : recorder_(std::move(recorder)) {
    *initial_event_.mutable_task() = std::move(task);
  }

  a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    if (!delivered_initial_) {
      delivered_initial_ = true;
      return std::optional<lf::a2a::v1::StreamResponse>(initial_event_);
    }

    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return cancelled_.load(); });
    return std::optional<lf::a2a::v1::StreamResponse>{};
  }

  [[nodiscard]] bool IsLive() const noexcept override { return !cancelled_.load(); }

  void Cancel() noexcept override {
    if (!cancelled_.exchange(true) && recorder_ != nullptr) {
      recorder_->RecordCancel();
    }
    changed_.notify_all();
  }

 private:
  std::shared_ptr<ControlledSubscriptionRecorder> recorder_;
  lf::a2a::v1::StreamResponse initial_event_;
  bool delivered_initial_ = false;
  std::atomic_bool cancelled_ = false;
  std::mutex mutex_;
  std::condition_variable changed_;
};

class StreamingStoreExecutor final : public a2a::server::AgentExecutor {
 public:
  explicit StreamingStoreExecutor(a2a::server::TaskStore* store) : store_(store) {}

  void RecordSubscribeCancellationFor(std::string task_id, std::shared_ptr<ControlledSubscriptionRecorder> recorder) {
    controlled_subscription_task_id_ = std::move(task_id);
    controlled_subscription_recorder_ = std::move(recorder);
  }

  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.message().task_id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    const auto saved = store_->CreateOrUpdate(task);
    if (!saved.ok()) {
      return saved.error();
    }
    subscriptions_.PublishTaskUpdated(task);
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task;
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(request.message().task_id());
    event.mutable_task()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    return std::unique_ptr<a2a::server::ServerStreamSession>(std::make_unique<StreamSession>(std::vector{event}));
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    return store_->Get(request.id());
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, a2a::server::RequestContext& context) override {
    auto task = GetTask(request, context);
    if (!task.ok()) {
      return task.error();
    }
    if (controlled_subscription_recorder_ != nullptr && request.id() == controlled_subscription_task_id_) {
      return std::unique_ptr<a2a::server::ServerStreamSession>(
          std::make_unique<ControlledSubscriptionSession>(task.value(), controlled_subscription_recorder_));
    }
    return subscriptions_.Subscribe(task.value());
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)context;
    return store_->List(request);
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    auto task = store_->Cancel(request.id());
    if (task.ok()) {
      subscriptions_.PublishTaskUpdated(task.value());
    }
    return task;
  }

 private:
  a2a::server::TaskStore* store_;
  a2a::server::TaskSubscriptionService subscriptions_;
  std::string controlled_subscription_task_id_;
  std::shared_ptr<ControlledSubscriptionRecorder> controlled_subscription_recorder_;
};

class RecordingObserver final : public a2a::client::StreamObserver {
 public:
  struct Snapshot final {
    std::vector<lf::a2a::v1::StreamResponse> events;
    std::vector<std::string> errors;
    int completed_count = 0;
  };

  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    {
      std::lock_guard lock(mutex_);
      events_.push_back(response);
    }
    changed_.notify_all();
  }

  void OnError(const a2a::core::Error& error) override {
    {
      std::lock_guard lock(mutex_);
      errors_.emplace_back(error.message());
    }
    changed_.notify_all();
  }

  void OnCompleted() override {
    {
      std::lock_guard lock(mutex_);
      ++completed_count_;
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool WaitForEventCount(std::size_t expected_count) const {
    constexpr auto kWaitTimeout = std::chrono::seconds(1);
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kWaitTimeout, [this, expected_count] { return events_.size() >= expected_count; });
  }

  [[nodiscard]] Snapshot GetSnapshot() const {
    std::lock_guard lock(mutex_);
    return Snapshot{.events = events_, .errors = errors_, .completed_count = completed_count_};
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::vector<std::string> errors_;
  int completed_count_ = 0;
};

struct GrpcServerHarness final {
  a2a::server::InMemoryTaskStore store;
  StreamingStoreExecutor executor{&store};
  lf::a2a::v1::AgentCard extended_card = [] {
    lf::a2a::v1::AgentCard card;
    card.set_name("A2A C++ SDK Agent");
    card.set_description("Default agent card for compatibility checks");
    card.set_version(std::string(a2a::core::Version::kAgentCardVersion));
    card.add_default_input_modes("text/plain");
    card.add_default_output_modes("text/plain");
    card.mutable_capabilities()->set_push_notifications(false);
    card.mutable_capabilities()->set_streaming(true);
    return card;
  }();
  std::shared_ptr<a2a::core::AgentCardProvider> agent_card_provider =
      std::make_shared<a2a::core::StaticAgentCardProvider>(extended_card);
  a2a::server::Dispatcher dispatcher{&executor, agent_card_provider};
  a2a::server::GrpcServerTransport transport{&dispatcher};
  std::unique_ptr<grpc::Server> server;
  int port = 0;
};

std::unique_ptr<GrpcServerHarness> StartHarness() {
  auto harness = std::make_unique<GrpcServerHarness>();
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &harness->port);
  builder.RegisterService(&harness->transport);
  harness->server = builder.BuildAndStart();
  return harness;
}

[[nodiscard]] a2a::core::Result<void> VerifyCoreLifecycle(a2a::client::A2AClient* client) {
  if (client == nullptr) {
    return a2a::core::Error::Internal("Client must not be null");
  }
  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  send_request.mutable_message()->set_task_id("grpc-integration-1");

  auto send_response = client->SendMessage(send_request);
  if (!send_response.ok()) {
    return send_response.error();
  }
  if (send_response.value().task().id() != "grpc-integration-1") {
    return a2a::core::Error::Internal("SendMessage returned unexpected task id");
  }

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("grpc-integration-1");
  auto get_response = client->GetTask(get_request);
  if (!get_response.ok()) {
    return get_response.error();
  }
  if (get_response.value().status().state() != lf::a2a::v1::TASK_STATE_WORKING) {
    return a2a::core::Error::Internal("GetTask returned unexpected state");
  }

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id("grpc-integration-1");
  auto cancel_response = client->CancelTask(cancel_request);
  if (!cancel_response.ok()) {
    return cancel_response.error();
  }
  if (cancel_response.value().status().state() != lf::a2a::v1::TASK_STATE_CANCELED) {
    return a2a::core::Error::Internal("CancelTask returned unexpected state");
  }
  return {};
}

[[nodiscard]] a2a::core::Result<void> VerifyStreamingMessage(a2a::client::A2AClient* client) {
  if (client == nullptr) {
    return a2a::core::Error::Internal("Client must not be null");
  }
  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  send_request.mutable_message()->set_task_id("grpc-integration-1");

  RecordingObserver observer;
  auto stream = client->SendStreamingMessage(send_request, observer);
  if (!stream.ok()) {
    return stream.error();
  }

  while (stream.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto snapshot = observer.GetSnapshot();
  if (snapshot.events.size() != 1U) {
    return a2a::core::Error::Internal("Unexpected streaming event count");
  }
  if (snapshot.events.front().task().id() != "grpc-integration-1") {
    return a2a::core::Error::Internal("Streaming event returned unexpected task id");
  }
  if (snapshot.completed_count == 0) {
    return a2a::core::Error::Internal("Streaming observer was not completed");
  }
  if (!snapshot.errors.empty()) {
    return a2a::core::Error::Internal("Streaming observer unexpectedly received errors");
  }
  return {};
}

std::unique_ptr<a2a::client::A2AClient> BuildClient(int port) {
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());

  auto transport = std::make_unique<a2a::client::GrpcTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kGrpc,
                                     .url = "127.0.0.1:" + std::to_string(port),
                                     .security_requirements = {},
                                     .security_schemes = {}},
      channel);
  return std::make_unique<a2a::client::A2AClient>(std::move(transport));
}

[[nodiscard]] a2a::core::Result<void> VerifyPushConfigUnsupported(a2a::client::A2AClient* client) {
  if (client == nullptr) {
    return a2a::core::Error::Internal("Client must not be null");
  }

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_id("missing-task-id");
  const auto get_response = client->GetTaskPushNotificationConfig(get_request);
  if (get_response.ok()) {
    return a2a::core::Error::Internal("Unsupported push-config get request should fail");
  }

  lf::a2a::v1::TaskPushNotificationConfig create_request;
  create_request.set_id("missing-task-id");
  const auto create_response = client->CreateTaskPushNotificationConfig(create_request);
  if (create_response.ok()) {
    return a2a::core::Error::Internal("Unsupported push-config create request should fail");
  }

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  const auto list_response = client->ListTaskPushNotificationConfigs(list_request);
  if (list_response.ok()) {
    return a2a::core::Error::Internal("Unsupported push-config list request should fail");
  }

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_id("missing-task-id");
  const auto delete_response = client->DeleteTaskPushNotificationConfig(delete_request);
  if (delete_response.ok()) {
    return a2a::core::Error::Internal("Unsupported push-config delete request should fail");
  }

  if (get_response.error().message().empty() || create_response.error().message().empty() ||
      list_response.error().message().empty() || delete_response.error().message().empty()) {
    return a2a::core::Error::Internal("Unsupported push-config errors should include messages");
  }

  return {};
}

[[nodiscard]] a2a::core::Result<void> VerifySubscribeTask(a2a::client::A2AClient* client) {
  if (client == nullptr) {
    return a2a::core::Error::Internal("Client must not be null");
  }

  constexpr std::string_view kTaskId = "grpc-subscribe-1";
  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  send_request.mutable_message()->set_task_id(std::string(kTaskId));
  const auto send_response = client->SendMessage(send_request);
  if (!send_response.ok()) {
    return send_response.error();
  }

  lf::a2a::v1::GetTaskRequest subscribe_request;
  subscribe_request.set_id(std::string(kTaskId));

  RecordingObserver observer;
  const auto stream = client->SubscribeTask(subscribe_request, observer);
  if (!stream.ok()) {
    return stream.error();
  }
  if (!observer.WaitForEventCount(1U)) {
    return a2a::core::Error::Internal("SubscribeTask did not produce its initial event");
  }

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id(std::string(kTaskId));
  const auto cancel_response = client->CancelTask(cancel_request);
  if (!cancel_response.ok()) {
    return cancel_response.error();
  }

  while (stream.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto snapshot = observer.GetSnapshot();
  if (!snapshot.errors.empty()) {
    return a2a::core::Error::Internal("SubscribeTask unexpectedly returned errors");
  }
  if (snapshot.completed_count == 0) {
    return a2a::core::Error::Internal("SubscribeTask stream did not complete");
  }
  constexpr std::size_t kExpectedSubscribeEventCount = 2U;
  if (snapshot.events.size() != kExpectedSubscribeEventCount) {
    return a2a::core::Error::Internal("SubscribeTask returned unexpected number of events");
  }
  if (!snapshot.events.front().has_task()) {
    return a2a::core::Error::Internal("SubscribeTask first event must contain task payload");
  }
  if (snapshot.events.front().task().id() != kTaskId) {
    return a2a::core::Error::Internal("SubscribeTask first event returned unexpected task id");
  }
  if (!snapshot.events[1].has_status_update()) {
    return a2a::core::Error::Internal("SubscribeTask second event must contain status_update payload");
  }
  if (snapshot.events[1].status_update().task_id() != kTaskId) {
    return a2a::core::Error::Internal("SubscribeTask second event returned unexpected task id");
  }
  return {};
}

[[nodiscard]] a2a::core::Result<void> VerifySubscribeTaskDeterministicOrdering(a2a::client::A2AClient* client) {
  if (client == nullptr) {
    return a2a::core::Error::Internal("Client must not be null");
  }

  constexpr std::string_view kTaskId = "grpc-subscribe-ordering-1";
  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  send_request.mutable_message()->set_task_id(std::string(kTaskId));
  const auto send_response = client->SendMessage(send_request);
  if (!send_response.ok()) {
    return send_response.error();
  }

  lf::a2a::v1::GetTaskRequest subscribe_request;
  subscribe_request.set_id(std::string(kTaskId));

  RecordingObserver first_observer;
  const auto first_stream = client->SubscribeTask(subscribe_request, first_observer);
  if (!first_stream.ok()) {
    return first_stream.error();
  }
  if (!first_observer.WaitForEventCount(1U)) {
    return a2a::core::Error::Internal("First SubscribeTask stream did not produce its initial event");
  }

  RecordingObserver second_observer;
  const auto second_stream = client->SubscribeTask(subscribe_request, second_observer);
  if (!second_stream.ok()) {
    return second_stream.error();
  }
  if (!second_observer.WaitForEventCount(1U)) {
    return a2a::core::Error::Internal("Second SubscribeTask stream did not produce its initial event");
  }

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id(std::string(kTaskId));
  const auto cancel_response = client->CancelTask(cancel_request);
  if (!cancel_response.ok()) {
    return cancel_response.error();
  }

  while (first_stream.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  while (second_stream.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  const auto first_snapshot = first_observer.GetSnapshot();
  const auto second_snapshot = second_observer.GetSnapshot();
  constexpr std::size_t kExpectedEventCount = 2U;
  if (first_snapshot.events.size() != kExpectedEventCount || second_snapshot.events.size() != kExpectedEventCount) {
    return a2a::core::Error::Internal("SubscribeTask streams returned unexpected event counts");
  }
  for (std::size_t index = 0; index < kExpectedEventCount; ++index) {
    if (first_snapshot.events[index].SerializeAsString() != second_snapshot.events[index].SerializeAsString()) {
      return a2a::core::Error::Internal("SubscribeTask streams emitted different events or ordering");
    }
  }
  return {};
}

[[nodiscard]] a2a::core::Result<void> VerifyListTasksValidation(a2a::client::A2AClient* client) {
  if (client == nullptr) {
    return a2a::core::Error::Internal("Client must not be null");
  }

  constexpr int32_t kInvalidPageSize = 101;
  a2a::client::ListTasksRequest invalid_page_size;
  invalid_page_size.page_size = static_cast<std::size_t>(kInvalidPageSize);
  const auto invalid_page_size_response = client->ListTasks(invalid_page_size);
  if (invalid_page_size_response.ok()) {
    return a2a::core::Error::Internal("ListTasks with invalid page_size should fail");
  }

  return {};
}

[[nodiscard]] a2a::core::Result<void> VerifyExtendedAgentCardRpc(int port) {
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
  auto stub = lf::a2a::v1::A2AService::NewStub(channel);
  grpc::ClientContext context;
  context.AddMetadata("a2a-version", "1.0");

  lf::a2a::v1::GetExtendedAgentCardRequest request;
  lf::a2a::v1::AgentCard response;
  const auto status = stub->GetExtendedAgentCard(&context, request, &response);
  if (!status.ok()) {
    return a2a::core::Error::Internal("GetExtendedAgentCard RPC failed");
  }
  if (response.name() != "A2A C++ SDK Agent") {
    return a2a::core::Error::Internal("Unexpected agent card name");
  }
  if (!response.capabilities().streaming() || response.capabilities().push_notifications()) {
    return a2a::core::Error::Internal("Unexpected capability defaults");
  }
  return {};
}

[[nodiscard]] a2a::core::Result<void> VerifyMissingTaskLookupFails(a2a::client::A2AClient* client) {
  if (client == nullptr) {
    return a2a::core::Error::Internal("Client must not be null");
  }

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("missing-grpc-task");
  const auto response = client->GetTask(request);
  if (response.ok()) {
    return a2a::core::Error::Internal("Missing task lookup should fail");
  }
  if (response.error().message().empty()) {
    return a2a::core::Error::Internal("Missing task failure should include an error message");
  }
  return {};
}

[[nodiscard]] a2a::core::Result<void> VerifyClientStreamCancellationCancelsServerSubscription(
    GrpcServerHarness* harness, a2a::client::A2AClient* client) {
  if (harness == nullptr || client == nullptr) {
    return a2a::core::Error::Internal("Harness and client must not be null");
  }

  auto recorder = std::make_shared<ControlledSubscriptionRecorder>();
  harness->executor.RecordSubscribeCancellationFor(std::string(kClientCancelledSubscribeTaskId), recorder);

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  send_request.mutable_message()->set_task_id(std::string(kClientCancelledSubscribeTaskId));
  const auto send_response = client->SendMessage(send_request);
  if (!send_response.ok()) {
    return send_response.error();
  }

  lf::a2a::v1::GetTaskRequest subscribe_request;
  subscribe_request.set_id(std::string(kClientCancelledSubscribeTaskId));
  RecordingObserver observer;
  auto stream = client->SubscribeTask(subscribe_request, observer);
  if (!stream.ok()) {
    return stream.error();
  }
  if (!observer.WaitForEventCount(1U)) {
    return a2a::core::Error::Internal("SubscribeTask did not deliver the initial event before client cancellation");
  }

  stream.value()->Cancel();
  if (!recorder->WaitForCancel()) {
    return a2a::core::Error::Internal("Server subscription did not observe client stream cancellation");
  }
  if (recorder->CancelCount() != 1) {
    return a2a::core::Error::Internal("Server subscription cancellation count was not exactly one");
  }
  if (stream.value()->IsActive()) {
    return a2a::core::Error::Internal("Client stream remained active after cancellation");
  }

  const auto snapshot = observer.GetSnapshot();
  if (snapshot.events.empty() || !snapshot.events.front().has_task()) {
    return a2a::core::Error::Internal("Client stream cancellation test did not receive initial task event");
  }
  if (snapshot.completed_count > 1 || snapshot.errors.size() > 1U) {
    return a2a::core::Error::Internal("Observer received duplicate terminal callbacks");
  }
  return {};
}

TEST(GrpcTransportIntegrationTest, ClientAndServerRoundTripCoreRpcsAndStreaming) {
  auto harness = StartHarness();
  ASSERT_NE(harness->server, nullptr);
  ASSERT_GT(harness->port, 0);

  auto client = BuildClient(harness->port);

  const auto lifecycle = VerifyCoreLifecycle(client.get());
  ASSERT_TRUE(lifecycle.ok()) << lifecycle.error().message();

  const auto streaming = VerifyStreamingMessage(client.get());
  ASSERT_TRUE(streaming.ok()) << streaming.error().message();

  harness->server->Shutdown();
}

TEST(GrpcTransportIntegrationTest, UnsupportedPushConfigMethodReturnsError) {
  auto harness = StartHarness();
  ASSERT_NE(harness->server, nullptr);
  ASSERT_GT(harness->port, 0);

  auto client = BuildClient(harness->port);
  const auto push_config = VerifyPushConfigUnsupported(client.get());
  ASSERT_TRUE(push_config.ok()) << push_config.error().message();

  harness->server->Shutdown();
}

TEST(GrpcTransportIntegrationTest, SubscribeTaskReturnsTaskEvents) {
  auto harness = StartHarness();
  ASSERT_NE(harness->server, nullptr);
  ASSERT_GT(harness->port, 0);

  auto client = BuildClient(harness->port);
  const auto subscribe = VerifySubscribeTask(client.get());
  ASSERT_TRUE(subscribe.ok()) << subscribe.error().message();

  harness->server->Shutdown();
}

TEST(GrpcTransportIntegrationTest, ClientStreamCancellationCancelsServerSubscription) {
  auto harness = StartHarness();
  ASSERT_NE(harness->server, nullptr);
  ASSERT_GT(harness->port, 0);

  auto client = BuildClient(harness->port);
  const auto cancellation = VerifyClientStreamCancellationCancelsServerSubscription(harness.get(), client.get());
  ASSERT_TRUE(cancellation.ok()) << cancellation.error().message();

  harness->server->Shutdown();
}

TEST(GrpcTransportIntegrationTest, SubscribeTaskIsDeterministicAcrossStreams) {
  auto harness = StartHarness();
  ASSERT_NE(harness, nullptr);
  auto client = BuildClient(harness->port);
  ASSERT_NE(client, nullptr);

  const auto subscribe = VerifySubscribeTaskDeterministicOrdering(client.get());
  ASSERT_TRUE(subscribe.ok()) << subscribe.error().message();
}

TEST(GrpcTransportIntegrationTest, ListTasksValidationErrorsAreReturned) {
  auto harness = StartHarness();
  ASSERT_NE(harness->server, nullptr);
  ASSERT_GT(harness->port, 0);

  auto client = BuildClient(harness->port);
  const auto validation = VerifyListTasksValidation(client.get());
  ASSERT_TRUE(validation.ok()) << validation.error().message();

  harness->server->Shutdown();
}

TEST(GrpcTransportIntegrationTest, GetExtendedAgentCardReturnsCompatibilityDefaults) {
  auto harness = StartHarness();
  ASSERT_NE(harness->server, nullptr);
  ASSERT_GT(harness->port, 0);

  const auto card = VerifyExtendedAgentCardRpc(harness->port);
  ASSERT_TRUE(card.ok()) << card.error().message();

  harness->server->Shutdown();
}

TEST(GrpcTransportIntegrationTest, GetTaskReturnsErrorForUnknownTaskId) {
  auto harness = StartHarness();
  ASSERT_NE(harness->server, nullptr);
  ASSERT_GT(harness->port, 0);

  auto client = BuildClient(harness->port);
  const auto missing_task = VerifyMissingTaskLookupFails(client.get());
  ASSERT_TRUE(missing_task.ok()) << missing_task.error().message();

  harness->server->Shutdown();
}

}  // namespace
