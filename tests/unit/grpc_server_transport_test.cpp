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
  bool fail_send = false;

  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    observed_remote_address = context.remote_address.value_or("");
    if (fail_send) {
      return a2a::core::Error::Validation("bad send").WithProtocolCode("invalid_request");
    }
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
    (void)context;
    observed_list_request = request;
    a2a::server::ListTasksResponse response;
    response.page_size = request.page_size;
    response.total_size = 1;
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  std::string observed_remote_address;
  a2a::server::ListTasksRequest observed_list_request;
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
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

TEST(GrpcServerTransportTest, ValidatesNullArgumentsAcrossRpcs) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::SendMessageRequest send;
  lf::a2a::v1::SendMessageResponse send_response;
  EXPECT_EQ(transport.SendMessage(&context, nullptr, &send_response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(transport.SendMessage(&context, &send, nullptr).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  lf::a2a::v1::GetTaskRequest get;
  lf::a2a::v1::Task task;
  EXPECT_EQ(transport.GetTask(&context, nullptr, &task).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(transport.GetTask(&context, &get, nullptr).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  lf::a2a::v1::CancelTaskRequest cancel;
  EXPECT_EQ(transport.CancelTask(&context, nullptr, &task).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(transport.CancelTask(&context, &cancel, nullptr).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(GrpcServerTransportTest, GetTaskCancelAndStreamingReturnExpectedPayloads) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::GetTaskRequest get;
  get.set_id("task-1");
  lf::a2a::v1::Task task;
  EXPECT_EQ(transport.GetTask(&context, &get, &task).error_code(), grpc::StatusCode::UNIMPLEMENTED);

  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id("task-2");
  lf::a2a::v1::Task canceled;
  EXPECT_EQ(transport.CancelTask(&context, &cancel, &canceled).error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

TEST(GrpcServerTransportTest, MapsDispatcherErrorToGrpcStatusAndMetadata) {
  FakeExecutor executor;
  executor.fail_send = true;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id("task-error");
  lf::a2a::v1::SendMessageResponse response;

  const auto status = transport.SendMessage(&context, &request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

TEST(GrpcServerTransportTest, ListTasksMapsAllSupportedFields) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::ListTasksRequest request;
  request.set_context_id("ctx-1");
  request.set_status(lf::a2a::v1::TASK_STATE_WORKING);
  request.set_page_token("2");
  request.set_page_size(3);
  request.set_history_length(2);
  request.set_include_artifacts(true);
  request.mutable_status_timestamp_after()->set_seconds(10);
  request.mutable_status_timestamp_after()->set_nanos(5);

  lf::a2a::v1::ListTasksResponse response;
  const auto status = transport.ListTasks(&context, &request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_EQ(executor.observed_list_request.context_id, "ctx-1");
  ASSERT_TRUE(executor.observed_list_request.status_filter.has_value());
  EXPECT_EQ(*executor.observed_list_request.status_filter, lf::a2a::v1::TASK_STATE_WORKING);
  EXPECT_EQ(executor.observed_list_request.page_token, "2");
  EXPECT_EQ(executor.observed_list_request.page_size, 3U);
  ASSERT_TRUE(executor.observed_list_request.history_length.has_value());
  EXPECT_EQ(*executor.observed_list_request.history_length, 2U);
  EXPECT_TRUE(executor.observed_list_request.include_artifacts);
  ASSERT_TRUE(executor.observed_list_request.status_timestamp_after.has_value());
  EXPECT_EQ(executor.observed_list_request.status_timestamp_after->seconds(), 10);
  EXPECT_EQ(executor.observed_list_request.status_timestamp_after->nanos(), 5);
}

}  // namespace
