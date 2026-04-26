#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "a2a/client/client.h"
#include "a2a/client/grpc_transport.h"

namespace {

class ContractRpcClient final : public a2a::client::GrpcTransport::RpcClient {
 public:
  grpc::Status SendMessage(grpc::ClientContext* context,
                           const lf::a2a::v1::SendMessageRequest& request,
                           lf::a2a::v1::SendMessageResponse* response) override {
    (void)context;
    response->mutable_task()->set_id(request.message().task_id());
    return grpc::Status::OK;
  }

  std::unique_ptr<a2a::client::GrpcTransport::StreamReader> SendStreamingMessage(
      grpc::ClientContext* context, const lf::a2a::v1::SendMessageRequest& request) override {
    (void)context;
    (void)request;
    return nullptr;
  }

  grpc::Status GetTask(grpc::ClientContext* context, const lf::a2a::v1::GetTaskRequest& request,
                       lf::a2a::v1::Task* response) override {
    (void)context;
    response->set_id(request.id());
    response->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    return grpc::Status::OK;
  }

  grpc::Status CancelTask(grpc::ClientContext* context,
                          const lf::a2a::v1::CancelTaskRequest& request,
                          lf::a2a::v1::Task* response) override {
    (void)context;
    response->set_id(request.id());
    response->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
    return grpc::Status::OK;
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
};

TEST(GrpcClientFunctionalTest, CoreRpcsRoundTripThroughClientContract) {
  auto transport = std::make_unique<a2a::client::GrpcTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kGrpc,
                                     .url = "localhost:50051",
                                     .security_requirements = {},
                                     .security_schemes = {}},
      std::make_unique<ContractRpcClient>());

  a2a::client::A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_task_id("grpc-functional-1");
  const auto send_result = client.SendMessage(send);
  ASSERT_TRUE(send_result.ok());
  EXPECT_EQ(send_result.value().task().id(), "grpc-functional-1");

  lf::a2a::v1::GetTaskRequest get;
  get.set_id("grpc-functional-1");
  const auto get_result = client.GetTask(get);
  ASSERT_TRUE(get_result.ok());
  EXPECT_EQ(get_result.value().status().state(), lf::a2a::v1::TASK_STATE_WORKING);

  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id("grpc-functional-1");
  const auto cancel_result = client.CancelTask(cancel);
  ASSERT_TRUE(cancel_result.ok());
  EXPECT_EQ(cancel_result.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

}  // namespace
