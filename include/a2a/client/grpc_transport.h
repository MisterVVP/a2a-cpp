// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/empty.pb.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/status.h>

#include <chrono>
#include <functional>
#include <memory>

#include "a2a/client/call_options.h"
#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/core/result.h"

#if defined(_WIN32) && defined(SendMessage)
#undef SendMessage
#endif

namespace a2a::client {

class GrpcTransport final : public ClientTransport {
 public:
  static constexpr std::chrono::milliseconds kDefaultTimeout{30000};
  ~GrpcTransport() override;
  [[nodiscard]] core::Result<void> Shutdown() override;

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
                                                     lf::a2a::v1::SendMessageResponse* response) = 0;

    [[nodiscard]] virtual std::unique_ptr<StreamReader> SendStreamingMessage(
        ::grpc::ClientContext* context, const lf::a2a::v1::SendMessageRequest& request) = 0;

    [[nodiscard]] virtual ::grpc::Status GetTask(::grpc::ClientContext* context,
                                                 const lf::a2a::v1::GetTaskRequest& request,
                                                 lf::a2a::v1::Task* response) = 0;

    [[nodiscard]] virtual ::grpc::Status ListTasks(::grpc::ClientContext* context,
                                                   const lf::a2a::v1::ListTasksRequest& request,
                                                   lf::a2a::v1::ListTasksResponse* response) = 0;

    [[nodiscard]] virtual std::unique_ptr<StreamReader> SubscribeToTask(
        ::grpc::ClientContext* context, const lf::a2a::v1::SubscribeToTaskRequest& request) {
      (void)context;
      (void)request;
      return std::unique_ptr<StreamReader>();
    }

    virtual void CancelStream(::grpc::ClientContext* context) {
      if (context != nullptr) {
        context->TryCancel();
      }
    }

    [[nodiscard]] virtual ::grpc::Status CancelTask(::grpc::ClientContext* context,
                                                    const lf::a2a::v1::CancelTaskRequest& request,
                                                    lf::a2a::v1::Task* response) = 0;

    [[nodiscard]] virtual ::grpc::Status CreateTaskPushNotificationConfig(
        ::grpc::ClientContext* context, const lf::a2a::v1::TaskPushNotificationConfig& request,
        lf::a2a::v1::TaskPushNotificationConfig* response) = 0;

    [[nodiscard]] virtual ::grpc::Status GetTaskPushNotificationConfig(
        ::grpc::ClientContext* context, const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
        lf::a2a::v1::TaskPushNotificationConfig* response) = 0;

    [[nodiscard]] virtual ::grpc::Status ListTaskPushNotificationConfigs(
        ::grpc::ClientContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
        lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) = 0;

    [[nodiscard]] virtual ::grpc::Status DeleteTaskPushNotificationConfig(
        ::grpc::ClientContext* context, const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
        google::protobuf::Empty* response) = 0;
    // Native async hook; sync-only injected clients report UNIMPLEMENTED.
    virtual void StartSendMessage(::grpc::ClientContext* context, const lf::a2a::v1::SendMessageRequest* request,
                                  lf::a2a::v1::SendMessageResponse* response,
                                  std::function<void(::grpc::Status)> completion) {
      (void)context;
      (void)request;
      (void)response;
      completion(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, kAsyncUnavailable));
    }
    virtual void StartGetTask(::grpc::ClientContext* context, const lf::a2a::v1::GetTaskRequest* request,
                              lf::a2a::v1::Task* response, std::function<void(::grpc::Status)> completion) {
      (void)context;
      (void)request;
      (void)response;
      completion(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, kAsyncUnavailable));
    }
    virtual void StartListTasks(::grpc::ClientContext* context, const lf::a2a::v1::ListTasksRequest* request,
                                lf::a2a::v1::ListTasksResponse* response,
                                std::function<void(::grpc::Status)> completion) {
      (void)context;
      (void)request;
      (void)response;
      completion(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, kAsyncUnavailable));
    }
    virtual void StartCancelTask(::grpc::ClientContext* context, const lf::a2a::v1::CancelTaskRequest* request,
                                 lf::a2a::v1::Task* response, std::function<void(::grpc::Status)> completion) {
      (void)context;
      (void)request;
      (void)response;
      completion(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, kAsyncUnavailable));
    }
    virtual void StartCreateTaskPushNotificationConfig(::grpc::ClientContext* context,
                                                       const lf::a2a::v1::TaskPushNotificationConfig* request,
                                                       lf::a2a::v1::TaskPushNotificationConfig* response,
                                                       std::function<void(::grpc::Status)> completion) {
      (void)context;
      (void)request;
      (void)response;
      completion(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, kAsyncUnavailable));
    }
    virtual void StartGetTaskPushNotificationConfig(::grpc::ClientContext* context,
                                                    const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
                                                    lf::a2a::v1::TaskPushNotificationConfig* response,
                                                    std::function<void(::grpc::Status)> completion) {
      (void)context;
      (void)request;
      (void)response;
      completion(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, kAsyncUnavailable));
    }
    virtual void StartListTaskPushNotificationConfigs(
        ::grpc::ClientContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
        lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response,
        std::function<void(::grpc::Status)> completion) {
      (void)context;
      (void)request;
      (void)response;
      completion(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, kAsyncUnavailable));
    }
    virtual void StartDeleteTaskPushNotificationConfig(
        ::grpc::ClientContext* context, const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
        google::protobuf::Empty* response, std::function<void(::grpc::Status)> completion) {
      (void)context;
      (void)request;
      (void)response;
      completion(::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, kAsyncUnavailable));
    }

   private:
    static constexpr const char* kAsyncUnavailable = "injected gRPC client does not implement native async unary calls";
  };

  GrpcTransport(ResolvedInterface resolved_interface, std::shared_ptr<::grpc::Channel> channel,
                std::chrono::milliseconds default_timeout = kDefaultTimeout);

  GrpcTransport(ResolvedInterface resolved_interface, std::unique_ptr<RpcClient> rpc_client,
                std::chrono::milliseconds default_timeout = kDefaultTimeout);

  [[nodiscard]] core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                        const CallOptions& options) override;
  [[nodiscard]] core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                          const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                           const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) override;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                          StreamObserver& observer,
                                                                          const CallOptions& options) override;

 protected:
  [[nodiscard]] std::chrono::milliseconds DefaultAsyncTimeout() const override { return default_timeout_; }
  void StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
                        const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) override;
  void StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                    const core::AsyncCompletion<lf::a2a::v1::Task>& completion) override;
  void StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                      const core::AsyncCompletion<ListTasksResponse>& completion) override;
  void StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                       const core::AsyncCompletion<lf::a2a::v1::Task>& completion) override;
  void StartCreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) override;
  void StartGetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) override;
  void StartListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) override;
  void StartDeleteTaskPushNotificationConfig(const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
                                             const CallOptions& options,
                                             const core::AsyncCompletion<void>& completion) override;

 private:
  [[nodiscard]] core::Result<std::unique_ptr<::grpc::ClientContext>> BuildContext(const CallOptions& options) const;
  [[nodiscard]] static core::Error BuildGrpcError(const ::grpc::Status& status);

  struct NativeCalls;
  std::shared_ptr<NativeCalls> native_calls_;
  ResolvedInterface resolved_interface_;
  std::shared_ptr<RpcClient> rpc_client_;
  std::chrono::milliseconds default_timeout_;
};

}  // namespace a2a::client
