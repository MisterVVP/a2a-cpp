// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/grpc_transport.h"
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

class StreamingStoreExecutor final : public a2a::server::AgentExecutor {
 public:
  explicit StreamingStoreExecutor(a2a::server::TaskStore* store) : store_(store) {}

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
};

class RecordingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override { events.push_back(response); }
  void OnError(const a2a::core::Error& error) override { errors.emplace_back(error.message()); }
  void OnCompleted() override { completed = true; }

  std::vector<lf::a2a::v1::StreamResponse> events;
  std::vector<std::string> errors;
  bool completed = false;
};

void WaitForObservedEvents(const RecordingObserver& observer, std::size_t expected_count) {
  constexpr int kMaxPolls = 200;
  constexpr auto kPollInterval = std::chrono::milliseconds(5);
  for (int poll = 0; poll < kMaxPolls && observer.events.size() < expected_count; ++poll) {
    std::this_thread::sleep_for(kPollInterval);
  }
}

struct GrpcServerHarness final {
  a2a::server::InMemoryTaskStore store;
  StreamingStoreExecutor executor{&store};
  a2a::server::Dispatcher dispatcher{&executor};
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

  if (observer.events.size() != 1U) {
    return a2a::core::Error::Internal("Unexpected streaming event count");
  }
  if (observer.events.front().task().id() != "grpc-integration-1") {
    return a2a::core::Error::Internal("Streaming event returned unexpected task id");
  }
  if (!observer.completed) {
    return a2a::core::Error::Internal("Streaming observer was not completed");
  }
  if (!observer.errors.empty()) {
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
  WaitForObservedEvents(observer, 1U);

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id(std::string(kTaskId));
  const auto cancel_response = client->CancelTask(cancel_request);
  if (!cancel_response.ok()) {
    return cancel_response.error();
  }

  while (stream.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (!observer.errors.empty()) {
    return a2a::core::Error::Internal("SubscribeTask unexpectedly returned errors");
  }
  if (!observer.completed) {
    return a2a::core::Error::Internal("SubscribeTask stream did not complete");
  }
  constexpr std::size_t kExpectedSubscribeEventCount = 2U;
  if (observer.events.size() != kExpectedSubscribeEventCount) {
    return a2a::core::Error::Internal("SubscribeTask returned unexpected number of events");
  }
  if (!observer.events.front().has_task()) {
    return a2a::core::Error::Internal("SubscribeTask first event must contain task payload");
  }
  if (observer.events.front().task().id() != kTaskId) {
    return a2a::core::Error::Internal("SubscribeTask first event returned unexpected task id");
  }
  if (!observer.events[1].has_status_update()) {
    return a2a::core::Error::Internal("SubscribeTask second event must contain status_update payload");
  }
  if (observer.events[1].status_update().task_id() != kTaskId) {
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
  WaitForObservedEvents(first_observer, 1U);

  RecordingObserver second_observer;
  const auto second_stream = client->SubscribeTask(subscribe_request, second_observer);
  if (!second_stream.ok()) {
    return second_stream.error();
  }
  WaitForObservedEvents(second_observer, 1U);

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

  constexpr std::size_t kExpectedEventCount = 2U;
  if (first_observer.events.size() != kExpectedEventCount || second_observer.events.size() != kExpectedEventCount) {
    return a2a::core::Error::Internal("SubscribeTask streams returned unexpected event counts");
  }
  for (std::size_t index = 0; index < kExpectedEventCount; ++index) {
    if (first_observer.events[index].SerializeAsString() != second_observer.events[index].SerializeAsString()) {
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
