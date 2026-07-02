// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "a2a/server/dispatcher.h"
#include "a2a/server/request_context.h"
#include "a2a/server/required_extensions_validator.h"
#include "a2a/v1/a2a.grpc.pb.h"

namespace a2a::server {

struct GrpcServerTransportOptions final {
  std::vector<std::string> required_extensions = {};
};

class GrpcServerTransport final : public lf::a2a::v1::A2AService::Service {
 public:
  static constexpr std::string_view kVersionMetadataKey = "a2a-version";
  static constexpr std::string_view kTransportName = "grpc";
  static constexpr std::string_view kProtocolCodeMetadataKey = "a2a-protocol-code";

  explicit GrpcServerTransport(Dispatcher* dispatcher, GrpcServerTransportOptions options = {});

  ::grpc::Status SendMessage(::grpc::ServerContext* context, const lf::a2a::v1::SendMessageRequest* request,
                             lf::a2a::v1::SendMessageResponse* response) override;

  ::grpc::Status SendStreamingMessage(::grpc::ServerContext* context, const lf::a2a::v1::SendMessageRequest* request,
                                      ::grpc::ServerWriter<lf::a2a::v1::StreamResponse>* writer) override;

  ::grpc::Status GetTask(::grpc::ServerContext* context, const lf::a2a::v1::GetTaskRequest* request,
                         lf::a2a::v1::Task* response) override;

  ::grpc::Status CancelTask(::grpc::ServerContext* context, const lf::a2a::v1::CancelTaskRequest* request,
                            lf::a2a::v1::Task* response) override;

  ::grpc::Status ListTasks(::grpc::ServerContext* context, const lf::a2a::v1::ListTasksRequest* request,
                           lf::a2a::v1::ListTasksResponse* response) override;
  ::grpc::Status SubscribeToTask(::grpc::ServerContext* context, const lf::a2a::v1::SubscribeToTaskRequest* request,
                                 ::grpc::ServerWriter<lf::a2a::v1::StreamResponse>* writer) override;

 private:
  struct ValidatedRequestContext final {
    RequestContext request_context;
    std::vector<std::string> activated_extensions;
  };

  [[nodiscard]] core::Result<ValidatedRequestContext> BuildRequestContext(const ::grpc::ServerContext& context) const;
  [[nodiscard]] static ::grpc::Status ToGrpcStatus(const core::Error& error, ::grpc::ServerContext* context,
                                                   const std::vector<std::string>& activated_extensions = {});

  ::grpc::Status CreateTaskPushNotificationConfig(::grpc::ServerContext* context,
                                                  const lf::a2a::v1::TaskPushNotificationConfig* request,
                                                  lf::a2a::v1::TaskPushNotificationConfig* response) override;

  ::grpc::Status GetTaskPushNotificationConfig(::grpc::ServerContext* context,
                                               const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
                                               lf::a2a::v1::TaskPushNotificationConfig* response) override;

  ::grpc::Status ListTaskPushNotificationConfigs(
      ::grpc::ServerContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
      lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) override;

  ::grpc::Status DeleteTaskPushNotificationConfig(::grpc::ServerContext* context,
                                                  const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
                                                  google::protobuf::Empty* response) override;

  ::grpc::Status GetExtendedAgentCard(::grpc::ServerContext* context,
                                      const lf::a2a::v1::GetExtendedAgentCardRequest* request,
                                      lf::a2a::v1::AgentCard* response) override;

  Dispatcher* dispatcher_ = nullptr;
  RequiredExtensionsValidator required_extensions_validator_;
};

}  // namespace a2a::server
