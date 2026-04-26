#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>
#include <google/protobuf/empty.pb.h>

#include <chrono>
#include <memory>

#include "a2a/client/call_options.h"
#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/core/result.h"

namespace a2a::client {

class GrpcTransport final : public ClientTransport {
 public:
  static constexpr std::chrono::milliseconds kDefaultTimeout{30000};

  class StreamReader {
   public:
    virtual ~StreamReader() = default;
    virtual bool Read(lf::a2a::v1::StreamResponse* response) = 0;
    [[nodiscard]] virtual ::grpc::Status Finish() = 0;
  };

  class RpcClient {
   public:
    virtual ~RpcClient() = default;

    [[nodiscard]] virtual ::grpc::Status SendMessage(::grpc::ClientContext* context,
                                                     const lf::a2a::v1::SendMessageRequest& request,
                                                     lf::a2a::v1::SendMessageResponse* response) =
        0;

    [[nodiscard]] virtual std::unique_ptr<StreamReader> SendStreamingMessage(
        ::grpc::ClientContext* context, const lf::a2a::v1::SendMessageRequest& request) = 0;

    [[nodiscard]] virtual ::grpc::Status GetTask(::grpc::ClientContext* context,
                                                 const lf::a2a::v1::GetTaskRequest& request,
                                                 lf::a2a::v1::Task* response) = 0;

    [[nodiscard]] virtual ::grpc::Status CancelTask(::grpc::ClientContext* context,
                                                    const lf::a2a::v1::CancelTaskRequest& request,
                                                    lf::a2a::v1::Task* response) = 0;

    [[nodiscard]] virtual ::grpc::Status SetTaskPushNotificationConfig(
        ::grpc::ClientContext* context, const lf::a2a::v1::TaskPushNotificationConfig& request,
        lf::a2a::v1::TaskPushNotificationConfig* response) = 0;

    [[nodiscard]] virtual ::grpc::Status GetTaskPushNotificationConfig(
        ::grpc::ClientContext* context,
        const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
        lf::a2a::v1::TaskPushNotificationConfig* response) = 0;

    [[nodiscard]] virtual ::grpc::Status ListTaskPushNotificationConfigs(
        ::grpc::ClientContext* context,
        const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
        lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) = 0;

    [[nodiscard]] virtual ::grpc::Status DeleteTaskPushNotificationConfig(
        ::grpc::ClientContext* context,
        const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
        google::protobuf::Empty* response) = 0;
  };

  GrpcTransport(ResolvedInterface resolved_interface, std::shared_ptr<::grpc::Channel> channel,
                std::chrono::milliseconds default_timeout = kDefaultTimeout);

  GrpcTransport(ResolvedInterface resolved_interface, std::unique_ptr<RpcClient> rpc_client,
                std::chrono::milliseconds default_timeout = kDefaultTimeout);

  [[nodiscard]] core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                        const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::Task> CancelTask(
      const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> SetTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
                                  const CallOptions& options) override;

  [[nodiscard]] core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const CallOptions& options) override;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
      const CallOptions& options) override;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer,
      const CallOptions& options) override;

 private:
  [[nodiscard]] core::Result<std::unique_ptr<::grpc::ClientContext>> BuildContext(
      const CallOptions& options) const;
  [[nodiscard]] core::Error BuildGrpcError(const ::grpc::Status& status) const;

  ResolvedInterface resolved_interface_;
  std::unique_ptr<RpcClient> rpc_client_;
  std::chrono::milliseconds default_timeout_;
};

}  // namespace a2a::client
