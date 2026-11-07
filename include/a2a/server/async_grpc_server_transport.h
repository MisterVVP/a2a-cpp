// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <grpcpp/server_context.h>
#include <grpcpp/support/server_callback.h>

#include <memory>

#include "a2a/core/non_copyable.h"
#include "a2a/server/engine/async_dispatcher.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/v1/a2a.grpc.pb.h"
namespace a2a::server {
// Register separately from GrpcServerTransport. Keep the service and dispatcher
// alive through server Wait(). Streaming uses one acknowledged write at a time.
class AsyncGrpcServerTransport final : public lf::a2a::v1::A2AService::CallbackService,
                                       private core::NonCopyableOrMovable {
 public:
  explicit AsyncGrpcServerTransport(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                    GrpcServerTransportOptions options = {});
  ~AsyncGrpcServerTransport() override;
  // Call before grpc::Server::Shutdown() to distinguish server shutdown from
  // client cancellation. Closes admission and queues cancellation without waiting.
  void Shutdown();
  ::grpc::ServerWriteReactor<lf::a2a::v1::StreamResponse>* SendStreamingMessage(
      ::grpc::CallbackServerContext* context, const lf::a2a::v1::SendMessageRequest* request) override;
  ::grpc::ServerWriteReactor<lf::a2a::v1::StreamResponse>* SubscribeToTask(
      ::grpc::CallbackServerContext* context, const lf::a2a::v1::SubscribeToTaskRequest* request) override;
  ::grpc::ServerUnaryReactor* SendMessage(::grpc::CallbackServerContext* context,
                                          const lf::a2a::v1::SendMessageRequest* request,
                                          lf::a2a::v1::SendMessageResponse* response) override;
  ::grpc::ServerUnaryReactor* GetTask(::grpc::CallbackServerContext* context,
                                      const lf::a2a::v1::GetTaskRequest* request, lf::a2a::v1::Task* response) override;
  ::grpc::ServerUnaryReactor* ListTasks(::grpc::CallbackServerContext* context,
                                        const lf::a2a::v1::ListTasksRequest* request,
                                        lf::a2a::v1::ListTasksResponse* response) override;
  ::grpc::ServerUnaryReactor* CancelTask(::grpc::CallbackServerContext* context,
                                         const lf::a2a::v1::CancelTaskRequest* request,
                                         lf::a2a::v1::Task* response) override;
  ::grpc::ServerUnaryReactor* CreateTaskPushNotificationConfig(
      ::grpc::CallbackServerContext* context, const lf::a2a::v1::TaskPushNotificationConfig* request,
      lf::a2a::v1::TaskPushNotificationConfig* response) override;
  ::grpc::ServerUnaryReactor* GetTaskPushNotificationConfig(
      ::grpc::CallbackServerContext* context, const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
      lf::a2a::v1::TaskPushNotificationConfig* response) override;
  ::grpc::ServerUnaryReactor* ListTaskPushNotificationConfigs(
      ::grpc::CallbackServerContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
      lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) override;
  ::grpc::ServerUnaryReactor* DeleteTaskPushNotificationConfig(
      ::grpc::CallbackServerContext* context, const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
      google::protobuf::Empty* response) override;
  ::grpc::ServerUnaryReactor* GetExtendedAgentCard(::grpc::CallbackServerContext* context,
                                                   const lf::a2a::v1::GetExtendedAgentCardRequest* request,
                                                   lf::a2a::v1::AgentCard* response) override;

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace a2a::server
