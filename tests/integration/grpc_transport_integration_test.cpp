#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/grpc_transport.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/server.h"

namespace {

class StreamSession final : public a2a::server::ServerStreamSession {
 public:
  explicit StreamSession(std::vector<lf::a2a::v1::StreamResponse> events)
      : events_(std::move(events)) {}

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

  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.message().task_id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    const auto saved = store_->CreateOrUpdate(task);
    if (!saved.ok()) {
      return saved.error();
    }
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task;
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(request.message().task_id());
    event.mutable_task()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    return std::unique_ptr<a2a::server::ServerStreamSession>(
        std::make_unique<StreamSession>(std::vector{event}));
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

class RecordingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override { events.push_back(response); }
  void OnError(const a2a::core::Error& error) override { errors.emplace_back(error.message()); }
  void OnCompleted() override { completed = true; }

  std::vector<lf::a2a::v1::StreamResponse> events;
  std::vector<std::string> errors;
  bool completed = false;
};

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
  send_request.mutable_message()->set_role("user");
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
  send_request.mutable_message()->set_role("user");
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
  auto channel =
      grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());

  auto transport = std::make_unique<a2a::client::GrpcTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kGrpc,
                                     .url = "127.0.0.1:" + std::to_string(port),
                                     .security_requirements = {},
                                     .security_schemes = {}},
      channel);
  return std::make_unique<a2a::client::A2AClient>(std::move(transport));
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

}  // namespace
