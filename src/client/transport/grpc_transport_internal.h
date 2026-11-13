// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include "a2a/client/async_transport.h"
#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/core/non_copyable.h"

namespace a2a::client::grpc_internal {
// Native channel streams share gRPC's callback workers instead of owning a reader thread.
class NativeStreams final : private core::NonCopyableOrMovable {
 public:
  explicit NativeStreams(const std::shared_ptr<::grpc::Channel>& channel);
  ~NativeStreams();
  [[nodiscard]] core::Result<std::shared_ptr<StreamHandle::State>> Start(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
      std::unique_ptr<::grpc::ClientContext> context);
  [[nodiscard]] core::Result<std::shared_ptr<StreamHandle::State>> Start(
      const lf::a2a::v1::SubscribeToTaskRequest& request, StreamObserver& observer,
      std::unique_ptr<::grpc::ClientContext> context);
  void Shutdown();

  struct State;

 private:
  std::shared_ptr<State> state_;
};
inline constexpr const char* kGetTaskIdRequired = "GetTaskRequest.id is required";
inline constexpr const char* kCancelTaskIdRequired = "CancelTaskRequest.id is required";
inline constexpr const char* kGetPushConfigIdRequired = "GetTaskPushNotificationConfigRequest.id is required";
inline constexpr const char* kDeletePushConfigIdRequired = "DeleteTaskPushNotificationConfigRequest.id is required";
[[nodiscard]] ListTasksResponse DecodeListTasks(lf::a2a::v1::ListTasksResponse&& response);
[[nodiscard]] core::Result<std::unique_ptr<::grpc::ClientContext>> BuildContext(
    const ResolvedInterface& resolved_interface, bool configured, std::chrono::milliseconds default_timeout,
    const CallOptions& options);
[[nodiscard]] core::Error BuildGrpcError(const ::grpc::Status& status);
[[nodiscard]] std::shared_ptr<AsyncClientTransport> MakeAsyncTransport(ResolvedInterface resolved_interface,
                                                                       std::shared_ptr<::grpc::Channel> channel,
                                                                       std::chrono::milliseconds default_timeout);
}  // namespace a2a::client::grpc_internal
