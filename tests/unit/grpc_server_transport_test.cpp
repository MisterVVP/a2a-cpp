// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/grpc_server_transport.h"

#if __has_include(<grpcpp/test/server_context_test_spouse.h>)
#include <grpcpp/test/server_context_test_spouse.h>
#define A2A_HAS_SERVER_CONTEXT_TEST_SPOUSE 1
#else
#define A2A_HAS_SERVER_CONTEXT_TEST_SPOUSE 0
#endif
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include "a2a/core/protocol_errors.h"
#include "a2a/core/version.h"
#include "a2a/server/server.h"

namespace {
constexpr int64_t kStatusTimestampSeconds = 10;
constexpr int32_t kStatusTimestampNanos = 5;
constexpr int32_t kValidPageSize = 10;
constexpr std::string_view kTaskIdOne = "task-1";
constexpr std::string_view kTaskIdTwo = "task-2";
constexpr std::string_view kSubscribeTaskId = "sub-task";

class FakeStreamSession final : public a2a::server::ServerStreamSession {
 public:
  explicit FakeStreamSession(std::vector<lf::a2a::v1::StreamResponse> events) : events_(std::move(events)) {}

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
  bool fail_get_task = false;
  bool fail_cancel_task = false;
  bool fail_list_tasks = false;

  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
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
      const lf::a2a::v1::SendMessageRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::StreamResponse event;
    event.mutable_task()->set_id(request.message().task_id());
    return std::unique_ptr<a2a::server::ServerStreamSession>(std::make_unique<FakeStreamSession>(std::vector{event}));
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    if (fail_get_task) {
      return a2a::core::protocol_errors::TaskNotFound(request.id());
    }
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(const a2a::server::ListTasksRequest& request,
                                                              a2a::server::RequestContext& context) override {
    (void)context;
    if (fail_list_tasks) {
      return a2a::core::protocol_errors::UnsupportedOperation("list not supported");
    }
    observed_list_request = request;
    a2a::server::ListTasksResponse response;
    response.page_size = request.page_size;
    response.total_size = 1;
    return response;
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    if (fail_cancel_task) {
      return a2a::core::protocol_errors::TaskNotCancelable(request.id());
    }
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  std::string observed_remote_address;
  a2a::server::ListTasksRequest observed_list_request;
};

TEST(GrpcServerTransportTest, ValidatesNullArgumentsAcrossRpcs) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::SendMessageRequest send;
  lf::a2a::v1::SendMessageResponse send_response;
  EXPECT_EQ(transport.SendMessage(&context, nullptr, &send_response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(transport.SendMessage(&context, &send, nullptr).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  lf::a2a::v1::GetTaskRequest get;
  lf::a2a::v1::Task task;
  EXPECT_EQ(transport.GetTask(&context, nullptr, &task).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(transport.GetTask(&context, &get, nullptr).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  lf::a2a::v1::CancelTaskRequest cancel;
  EXPECT_EQ(transport.CancelTask(&context, nullptr, &task).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(transport.CancelTask(&context, &cancel, nullptr).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  lf::a2a::v1::ListTasksRequest list_tasks;
  lf::a2a::v1::ListTasksResponse list_response;
  EXPECT_EQ(transport.ListTasks(&context, nullptr, &list_response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(transport.ListTasks(&context, &list_tasks, nullptr).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  lf::a2a::v1::GetExtendedAgentCardRequest card_request;
  lf::a2a::v1::AgentCard card_response;
  auto* service = static_cast<lf::a2a::v1::A2AService::Service*>(&transport);
  EXPECT_EQ(service->GetExtendedAgentCard(&context, nullptr, &card_response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(service->GetExtendedAgentCard(&context, &card_request, nullptr).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

#if A2A_HAS_SERVER_CONTEXT_TEST_SPOUSE
void AddValidVersionHeader(grpc::testing::ServerContextTestSpouse& spouse) {
  spouse.AddClientMetadata(std::string(a2a::server::GrpcServerTransport::kVersionMetadataKey),
                           a2a::core::Version::HeaderValue());
}

TEST(GrpcServerTransportTest, SendMessageSuccessWithVersionHeader) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  grpc::testing::ServerContextTestSpouse spouse(&context);
  AddValidVersionHeader(spouse);
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id(std::string(kTaskIdOne));
  lf::a2a::v1::SendMessageResponse response;

  const auto status = transport.SendMessage(&context, &request, &response);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(response.task().id(), std::string(kTaskIdOne));
}

TEST(GrpcServerTransportTest, DispatchErrorMapsProtocolCodeAndTrailingMetadata) {
  FakeExecutor executor;
  executor.fail_send = true;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  grpc::testing::ServerContextTestSpouse spouse(&context);
  AddValidVersionHeader(spouse);
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id(std::string(kTaskIdOne));
  lf::a2a::v1::SendMessageResponse response;

  const auto status = transport.SendMessage(&context, &request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  const auto trailing = spouse.GetTrailingMetadata();
  EXPECT_TRUE(trailing.contains("a2a-error-code"));
  EXPECT_TRUE(trailing.contains(std::string(a2a::server::GrpcServerTransport::kProtocolCodeMetadataKey)));
  EXPECT_TRUE(trailing.contains("grpc-status-details-bin"));
}

TEST(GrpcServerTransportTest, GetTaskNotFoundMapsToGrpcNotFound) {
  FakeExecutor executor;
  executor.fail_get_task = true;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  grpc::testing::ServerContextTestSpouse spouse(&context);
  AddValidVersionHeader(spouse);
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kTaskIdOne));
  lf::a2a::v1::Task response;

  const auto status = transport.GetTask(&context, &request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST(GrpcServerTransportTest, CancelTaskProtocolErrorMapsToFailedPrecondition) {
  FakeExecutor executor;
  executor.fail_cancel_task = true;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  grpc::testing::ServerContextTestSpouse spouse(&context);
  AddValidVersionHeader(spouse);
  lf::a2a::v1::CancelTaskRequest request;
  request.set_id(std::string(kTaskIdOne));
  lf::a2a::v1::Task response;

  const auto status = transport.CancelTask(&context, &request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
}

TEST(GrpcServerTransportTest, ListTasksValidatesPageSizeAndHistoryLength) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  grpc::testing::ServerContextTestSpouse spouse(&context);
  AddValidVersionHeader(spouse);
  lf::a2a::v1::ListTasksRequest request;
  request.set_page_size(0);
  lf::a2a::v1::ListTasksResponse response;
  EXPECT_EQ(transport.ListTasks(&context, &request, &response).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  lf::a2a::v1::ListTasksRequest request_large_page_size;
  constexpr int32_t kInvalidLargePageSize = 101;
  request_large_page_size.set_page_size(kInvalidLargePageSize);
  EXPECT_EQ(transport.ListTasks(&context, &request_large_page_size, &response).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(GrpcServerTransportTest, ListTasksCopiesResponseFieldsOnSuccess) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  grpc::testing::ServerContextTestSpouse spouse(&context);
  AddValidVersionHeader(spouse);
  lf::a2a::v1::ListTasksRequest request;
  request.set_page_size(3);
  request.set_page_token("token-1");
  request.set_context_id("ctx-1");
  request.set_history_length(2);
  request.set_include_artifacts(true);
  request.set_status(lf::a2a::v1::TASK_STATE_WORKING);
  lf::a2a::v1::ListTasksResponse response;

  const auto status = transport.ListTasks(&context, &request, &response);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(response.page_size(), 3);
  EXPECT_EQ(response.total_size(), 1);
  EXPECT_EQ(executor.observed_list_request.page_token, "token-1");
  EXPECT_EQ(executor.observed_list_request.context_id, "ctx-1");
  EXPECT_TRUE(executor.observed_list_request.include_artifacts);
}

#endif  // A2A_HAS_SERVER_CONTEXT_TEST_SPOUSE

TEST(GrpcServerTransportTest, MissingVersionHeaderReturnsUnimplementedForUnaryOperations) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_task_id(std::string(kTaskIdOne));
  lf::a2a::v1::SendMessageResponse send_response;
  EXPECT_EQ(transport.SendMessage(&context, &send, &send_response).error_code(), grpc::StatusCode::UNIMPLEMENTED);

  lf::a2a::v1::GetTaskRequest get;
  get.set_id(std::string(kTaskIdOne));
  lf::a2a::v1::Task get_response;
  EXPECT_EQ(transport.GetTask(&context, &get, &get_response).error_code(), grpc::StatusCode::UNIMPLEMENTED);

  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id(std::string(kTaskIdTwo));
  lf::a2a::v1::Task cancel_response;
  EXPECT_EQ(transport.CancelTask(&context, &cancel, &cancel_response).error_code(), grpc::StatusCode::UNIMPLEMENTED);

  lf::a2a::v1::ListTasksRequest list;
  list.set_page_size(kValidPageSize);
  lf::a2a::v1::ListTasksResponse list_response;
  EXPECT_EQ(transport.ListTasks(&context, &list, &list_response).error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

TEST(GrpcServerTransportTest, MissingVersionHeaderReturnsUnimplementedForStreamingOperations) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_task_id(std::string(kTaskIdOne));
  EXPECT_EQ(transport.SendStreamingMessage(&context, &send, nullptr).error_code(), grpc::StatusCode::INVALID_ARGUMENT);

  lf::a2a::v1::SubscribeToTaskRequest subscribe;
  subscribe.set_id(std::string(kSubscribeTaskId));
  EXPECT_EQ(transport.SubscribeToTask(&context, &subscribe, nullptr).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(GrpcServerTransportTest, ListTasksInputValidationRequiresProtocolVersionHeader) {
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
  request.mutable_status_timestamp_after()->set_seconds(kStatusTimestampSeconds);
  request.mutable_status_timestamp_after()->set_nanos(kStatusTimestampNanos);

  lf::a2a::v1::ListTasksResponse response;
  const auto status = transport.ListTasks(&context, &request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
}

TEST(GrpcServerTransportTest, PushNotificationRpcsReturnUnimplemented) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);
  grpc::ServerContext context;

  lf::a2a::v1::TaskPushNotificationConfig create_request;
  lf::a2a::v1::TaskPushNotificationConfig create_response;
  auto* service = static_cast<lf::a2a::v1::A2AService::Service*>(&transport);
  EXPECT_EQ(service->CreateTaskPushNotificationConfig(&context, &create_request, &create_response).error_code(),
            grpc::StatusCode::UNIMPLEMENTED);

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  lf::a2a::v1::TaskPushNotificationConfig get_response;
  EXPECT_EQ(service->GetTaskPushNotificationConfig(&context, &get_request, &get_response).error_code(),
            grpc::StatusCode::UNIMPLEMENTED);

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  lf::a2a::v1::ListTaskPushNotificationConfigsResponse list_response;
  EXPECT_EQ(service->ListTaskPushNotificationConfigs(&context, &list_request, &list_response).error_code(),
            grpc::StatusCode::UNIMPLEMENTED);

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  google::protobuf::Empty delete_response;
  EXPECT_EQ(service->DeleteTaskPushNotificationConfig(&context, &delete_request, &delete_response).error_code(),
            grpc::StatusCode::UNIMPLEMENTED);
}

TEST(GrpcServerTransportTest, GetExtendedAgentCardProvidesCompatibilityDefaults) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::GetExtendedAgentCardRequest request;
  lf::a2a::v1::AgentCard response;

  auto* service = static_cast<lf::a2a::v1::A2AService::Service*>(&transport);
  const auto status = service->GetExtendedAgentCard(&context, &request, &response);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(response.name(), "A2A C++ SDK Agent");
  EXPECT_EQ(response.description(), "Default agent card for compatibility checks");
  EXPECT_EQ(response.version(), a2a::core::Version::kAgentCardVersion);
  ASSERT_EQ(response.default_input_modes_size(), 1);
  ASSERT_EQ(response.default_output_modes_size(), 1);
  EXPECT_EQ(response.default_input_modes(0), "text/plain");
  EXPECT_EQ(response.default_output_modes(0), "text/plain");
  EXPECT_FALSE(response.capabilities().push_notifications());
  EXPECT_TRUE(response.capabilities().streaming());
}

TEST(GrpcServerTransportTest, ReturnsInternalWhenDispatcherMissing) {
  a2a::server::GrpcServerTransport transport(nullptr);
  grpc::ServerContext context;

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_task_id(std::string(kTaskIdOne));
  lf::a2a::v1::SendMessageResponse send_response;
  const auto send_status = transport.SendMessage(&context, &send_request, &send_response);
  EXPECT_EQ(send_status.error_code(), grpc::StatusCode::INTERNAL);

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id(std::string(kTaskIdOne));
  lf::a2a::v1::Task get_response;
  const auto get_status = transport.GetTask(&context, &get_request, &get_response);
  EXPECT_EQ(get_status.error_code(), grpc::StatusCode::INTERNAL);

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id(std::string(kTaskIdOne));
  lf::a2a::v1::Task cancel_response;
  const auto cancel_status = transport.CancelTask(&context, &cancel_request, &cancel_response);
  EXPECT_EQ(cancel_status.error_code(), grpc::StatusCode::INTERNAL);

  lf::a2a::v1::ListTasksRequest list_request;
  list_request.set_page_size(kValidPageSize);
  lf::a2a::v1::ListTasksResponse list_response;
  const auto list_status = transport.ListTasks(&context, &list_request, &list_response);
  EXPECT_EQ(list_status.error_code(), grpc::StatusCode::INTERNAL);
}

TEST(GrpcServerTransportTest, PushNotificationMethodsReturnProtocolMessage) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);
  grpc::ServerContext context;
  auto* service = static_cast<lf::a2a::v1::A2AService::Service*>(&transport);

  lf::a2a::v1::TaskPushNotificationConfig create_request;
  lf::a2a::v1::TaskPushNotificationConfig create_response;
  const auto create_status = service->CreateTaskPushNotificationConfig(&context, &create_request, &create_response);
  EXPECT_EQ(create_status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_FALSE(create_status.error_message().empty());

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  lf::a2a::v1::TaskPushNotificationConfig get_response;
  const auto get_status = service->GetTaskPushNotificationConfig(&context, &get_request, &get_response);
  EXPECT_EQ(get_status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_FALSE(get_status.error_message().empty());
}

TEST(GrpcServerTransportTest, StreamingRpcsValidateNullRequestPointers) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  EXPECT_EQ(transport.SendStreamingMessage(&context, nullptr, nullptr).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(transport.SubscribeToTask(&context, nullptr, nullptr).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(GrpcServerTransportTest, MissingVersionHeaderIncludesHelpfulMessage) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);

  grpc::ServerContext context;
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kTaskIdOne));
  lf::a2a::v1::Task response;

  const auto status = transport.GetTask(&context, &request, &response);
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_NE(status.error_message().find("Missing required A2A-Version header"), std::string::npos);
}

TEST(GrpcServerTransportTest, InvalidArgumentMessagesAreStableForRpcShapes) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);
  grpc::ServerContext context;

  lf::a2a::v1::SendMessageResponse send_response;
  const auto send_status = transport.SendMessage(&context, nullptr, &send_response);
  EXPECT_EQ(send_status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(send_status.error_message(), "Request and response are required");

  const auto streaming_status = transport.SendStreamingMessage(&context, nullptr, nullptr);
  EXPECT_EQ(streaming_status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(streaming_status.error_message(), "Request and writer are required");

  lf::a2a::v1::Task task_response;
  const auto get_status = transport.GetTask(&context, nullptr, &task_response);
  EXPECT_EQ(get_status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(get_status.error_message(), "Request and response are required");

  const auto subscribe_status = transport.SubscribeToTask(&context, nullptr, nullptr);
  EXPECT_EQ(subscribe_status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(subscribe_status.error_message(), "Request and writer are required");
}

TEST(GrpcServerTransportTest, MissingVersionHeaderMappingIsConsistentAcrossRpcs) {
  FakeExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::GrpcServerTransport transport(&dispatcher);
  grpc::ServerContext context;

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_task_id(std::string(kTaskIdOne));
  lf::a2a::v1::SendMessageResponse send_response;
  const auto send_status = transport.SendMessage(&context, &send_request, &send_response);
  EXPECT_EQ(send_status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_NE(send_status.error_message().find("Missing required A2A-Version header"), std::string::npos);

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id(std::string(kTaskIdTwo));
  lf::a2a::v1::Task cancel_response;
  const auto cancel_status = transport.CancelTask(&context, &cancel_request, &cancel_response);
  EXPECT_EQ(cancel_status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_NE(cancel_status.error_message().find("Missing required A2A-Version header"), std::string::npos);

  lf::a2a::v1::ListTasksRequest list_request;
  lf::a2a::v1::ListTasksResponse list_response;
  const auto list_status = transport.ListTasks(&context, &list_request, &list_response);
  EXPECT_EQ(list_status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_NE(list_status.error_message().find("Missing required A2A-Version header"), std::string::npos);
}

}  // namespace
