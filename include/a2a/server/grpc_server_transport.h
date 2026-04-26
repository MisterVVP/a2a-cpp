#pragma once

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include <string>
#include <unordered_map>

#include "a2a/server/server.h"
#include "a2a/v1/a2a.grpc.pb.h"

namespace a2a::server {

class GrpcServerTransport final : public lf::a2a::v1::A2AService::Service {
 public:
  explicit GrpcServerTransport(Dispatcher* dispatcher);

  ::grpc::Status SendMessage(::grpc::ServerContext* context,
                             const lf::a2a::v1::SendMessageRequest* request,
                             lf::a2a::v1::SendMessageResponse* response) override;

  ::grpc::Status SendStreamingMessage(
      ::grpc::ServerContext* context, const lf::a2a::v1::SendMessageRequest* request,
      ::grpc::ServerWriter<lf::a2a::v1::StreamResponse>* writer) override;

  ::grpc::Status GetTask(::grpc::ServerContext* context, const lf::a2a::v1::GetTaskRequest* request,
                         lf::a2a::v1::Task* response) override;

  ::grpc::Status CancelTask(::grpc::ServerContext* context,
                            const lf::a2a::v1::CancelTaskRequest* request,
                            lf::a2a::v1::Task* response) override;

 private:
  [[nodiscard]] core::Result<RequestContext> BuildRequestContext(
      const ::grpc::ServerContext& context) const;
  [[nodiscard]] static ::grpc::Status ToGrpcStatus(const core::Error& error,
                                                   ::grpc::ServerContext* context);

  ::grpc::Status SetTaskPushNotificationConfig(
      ::grpc::ServerContext* context, const lf::a2a::v1::TaskPushNotificationConfig* request,
      lf::a2a::v1::TaskPushNotificationConfig* response) override;

  ::grpc::Status GetTaskPushNotificationConfig(
      ::grpc::ServerContext* context,
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
      lf::a2a::v1::TaskPushNotificationConfig* response) override;

  ::grpc::Status ListTaskPushNotificationConfigs(
      ::grpc::ServerContext* context,
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
      lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) override;

  ::grpc::Status DeleteTaskPushNotificationConfig(
      ::grpc::ServerContext* context,
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
      google::protobuf::Empty* response) override;

  Dispatcher* dispatcher_ = nullptr;
};

}  // namespace a2a::server
