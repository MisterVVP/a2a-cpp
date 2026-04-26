#include "a2a/server/grpc_server_transport.h"

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <vector>

#include "a2a/server/server.h"

namespace {

class FakeStreamSession final : public a2a::server::ServerStreamSession {
 public:
  explicit FakeStreamSession(std::vector<lf::a2a::v1::StreamResponse> events)
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

class FakeExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    observed_remote_address = context.remote_address.value_or("");
    lf::a2a::v1::SendMessageResponse response;
    response.mutable_task()->set_id(request.message().task_id());
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(request.message().task_id());
    return std::unique_ptr<a2a::server::ServerStreamSession>(
        std::make_unique<FakeStreamSession>(std::vector{event}));
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(
      const a2a::server::ListTasksRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::server::ListTasksResponse{};
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  std::string observed_remote_address;
};

TEST(GrpcServerTransportTest, SendMessageDispatchesAndExtractsAuthMetadata) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id("grpc-server-unit-1");
  lf::a2a::v1::SendMessageResponse response;

  const auto status = transport.SendMessage(&context, &request, &response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(response.task().id(), "grpc-server-unit-1");
}

}  // namespace
