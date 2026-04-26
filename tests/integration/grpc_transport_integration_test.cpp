#include <gtest/gtest.h>

#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

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
  void OnError(const a2a::core::Error& error) override {
    errors.push_back(std::string(error.message()));
  }
  void OnCompleted() override { completed = true; }

  std::vector<lf::a2a::v1::StreamResponse> events;
  std::vector<std::string> errors;
  bool completed = false;
};

TEST(GrpcTransportIntegrationTest, ClientAndServerRoundTripCoreRpcsAndStreaming) {
  a2a::server::InMemoryTaskStore store;
  StreamingStoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerBuilder builder;
  int selected_port = 0;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &selected_port);
  builder.RegisterService(&transport);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  ASSERT_GT(selected_port, 0);

  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(selected_port),
                                     grpc::InsecureChannelCredentials());

  a2a::client::GrpcTransport grpc_client(
      {.transport = a2a::client::PreferredTransport::kGrpc,
       .url = "127.0.0.1:" + std::to_string(selected_port),
       .security_requirements = {},
       .security_schemes = {}},
      channel);
  a2a::client::A2AClient client(std::make_unique<a2a::client::GrpcTransport>(std::move(grpc_client)));

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role("user");
  send_request.mutable_message()->set_task_id("grpc-integration-1");

  const auto send_response = client.SendMessage(send_request);
  ASSERT_TRUE(send_response.ok()) << send_response.error().message();
  EXPECT_EQ(send_response.value().task().id(), "grpc-integration-1");

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("grpc-integration-1");
  const auto get_response = client.GetTask(get_request);
  ASSERT_TRUE(get_response.ok()) << get_response.error().message();
  EXPECT_EQ(get_response.value().status().state(), lf::a2a::v1::TASK_STATE_WORKING);

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id("grpc-integration-1");
  const auto cancel_response = client.CancelTask(cancel_request);
  ASSERT_TRUE(cancel_response.ok()) << cancel_response.error().message();
  EXPECT_EQ(cancel_response.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);

  RecordingObserver observer;
  auto stream = client.SendStreamingMessage(send_request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();

  while (stream.value()->IsActive()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_EQ(observer.events.size(), 1U);
  EXPECT_EQ(observer.events.front().task().id(), "grpc-integration-1");
  EXPECT_TRUE(observer.completed);
  EXPECT_TRUE(observer.errors.empty());

  server->Shutdown();
}

}  // namespace
