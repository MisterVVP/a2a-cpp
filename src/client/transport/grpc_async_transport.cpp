// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "a2a/v1/a2a.grpc.pb.h"
#include "grpc_transport_internal.h"
namespace a2a::client {
namespace {
using grpc_internal::BuildGrpcError;
constexpr const char* kGrpcShutdown = "gRPC async transport is shut down";
constexpr const char* kGrpcAsyncTransport = "grpc";
using grpc_internal::kCancelTaskIdRequired;
using grpc_internal::kDeletePushConfigIdRequired;
using grpc_internal::kGetPushConfigIdRequired;
using grpc_internal::kGetTaskIdRequired;
struct NativeCalls final {
  std::mutex mutex;
  std::condition_variable finished;
  std::size_t active = 0;
  bool closed = false;
};
template <typename Request, typename Response>
struct AsyncGrpcCall final : private core::NonCopyableOrMovable {
  ~AsyncGrpcCall() {
    // Native resources must die before a shutdown waiter sees the lease released.
    context.reset();
    stub.reset();
    if (owner) {
      {
        std::lock_guard lock(owner->mutex);
        --owner->active;
      }
      owner->finished.notify_all();
    }
  }
  bool Register(const std::shared_ptr<NativeCalls>& state) {
    std::lock_guard lock(state->mutex);
    if (state->closed) {
      return false;
    }
    // Shared-pointer assignment cannot allocate; registration is exception-safe.
    owner = state;
    ++state->active;
    return true;
  }
  std::shared_ptr<NativeCalls> owner;
  std::shared_ptr<::grpc::ClientContext> context;
  std::shared_ptr<lf::a2a::v1::A2AService::StubInterface> stub;
  Request request;
  Response response;
};
struct DecodeResponse final {
  template <typename T>
  core::Result<T> operator()(T response) const {
    return response;
  }
};
struct DiscardResponse final {
  core::Result<void> operator()(google::protobuf::Empty&& response) const {
    (void)response;
    return {};
  }
};
class GrpcAsyncTransport final : public AsyncClientTransport {
 public:
  GrpcAsyncTransport(ResolvedInterface resolved_interface, std::shared_ptr<::grpc::Channel> channel,
                     std::chrono::milliseconds timeout)
      : resolved_interface_(std::move(resolved_interface)),
        default_timeout_(timeout),
        stub_(lf::a2a::v1::A2AService::NewStub(std::move(channel))) {}
  ~GrpcAsyncTransport() override { Shutdown(); }
  void Shutdown() override {
    {
      std::lock_guard lock(native_calls_->mutex);
      native_calls_->closed = true;
    }
    async_scope_.Shutdown();
    std::unique_lock lock(native_calls_->mutex);
    native_calls_->finished.wait(lock, [this] { return native_calls_->active == 0U; });
  }

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
  template <typename Response, typename Request, typename T, typename Invoke, typename Decode = DecodeResponse>
  void StartRpc(const Request& request, const CallOptions& options, const core::AsyncCompletion<T>& completion,
                const Invoke& invoke, Decode decode = {}) {
    auto context = grpc_internal::BuildContext(resolved_interface_, stub_ != nullptr, default_timeout_, options);
    if (!context.ok()) {
      completion.Complete(context.error());
      return;
    }
    auto call = std::make_shared<AsyncGrpcCall<Request, Response>>();
    call->context = std::move(context.value());
    call->stub = stub_;
    call->request = request;
    if (!call->Register(native_calls_)) {
      completion.Complete(core::Error::Network(kGrpcShutdown).WithProtocolCode(core::AsyncHandle::kCancelled));
      return;
    }
    completion.RegisterCancellation([weak_context = std::weak_ptr(call->context)] {
      if (auto active = weak_context.lock()) {
        active->TryCancel();
      }
    });
    invoke(call->stub->async(), call->context.get(), &call->request, &call->response,
           [call, completion, decode = std::move(decode)](const ::grpc::Status& status) {
             if (status.error_code() == ::grpc::StatusCode::DEADLINE_EXCEEDED) {
               completion.Complete(core::Error::Network(status.error_message())
                                       .WithTransport(kGrpcAsyncTransport)
                                       .WithProtocolCode(core::AsyncHandle::kDeadlineExceeded));
               return;
             }
             if (!status.ok()) {
               completion.Complete(BuildGrpcError(status));
               return;
             }
             completion.Complete(decode(std::move(call->response)));
           });
  }
  std::shared_ptr<NativeCalls> native_calls_ = std::make_shared<NativeCalls>();
  ResolvedInterface resolved_interface_;
  std::chrono::milliseconds default_timeout_;
  std::shared_ptr<lf::a2a::v1::A2AService::StubInterface> stub_;
};
void GrpcAsyncTransport::StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
                                          const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) {
  StartRpc<lf::a2a::v1::SendMessageResponse>(request, options, completion,
                                             [](auto* stub, auto* context, auto* input, auto* output, auto done) {
                                               stub->SendMessage(context, input, output, std::move(done));
                                             });
}
void GrpcAsyncTransport::StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                                      const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  if (request.id().empty()) {
    completion.Complete(core::Error::Validation(kGetTaskIdRequired));
    return;
  }
  StartRpc<lf::a2a::v1::Task>(request, options, completion,
                              [](auto* stub, auto* context, auto* input, auto* output, auto done) {
                                stub->GetTask(context, input, output, std::move(done));
                              });
}
void GrpcAsyncTransport::StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                                        const core::AsyncCompletion<ListTasksResponse>& completion) {
  lf::a2a::v1::ListTasksRequest rpc_request;
  if (request.page_size > 0) {
    rpc_request.set_page_size(static_cast<std::int32_t>(request.page_size));
  }
  rpc_request.set_page_token(request.page_token);
  StartRpc<lf::a2a::v1::ListTasksResponse>(
      rpc_request, options, completion,
      [](auto* stub, auto* context, auto* input, auto* output, auto done) {
        stub->ListTasks(context, input, output, std::move(done));
      },
      grpc_internal::DecodeListTasks);
}
void GrpcAsyncTransport::StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                                         const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  if (request.id().empty()) {
    completion.Complete(core::Error::Validation(kCancelTaskIdRequired));
    return;
  }
  StartRpc<lf::a2a::v1::Task>(request, options, completion,
                              [](auto* stub, auto* context, auto* input, auto* output, auto done) {
                                stub->CancelTask(context, input, output, std::move(done));
                              });
}
void GrpcAsyncTransport::StartCreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  StartRpc<lf::a2a::v1::TaskPushNotificationConfig>(
      request, options, completion, [](auto* stub, auto* context, auto* input, auto* output, auto done) {
        stub->CreateTaskPushNotificationConfig(context, input, output, std::move(done));
      });
}
void GrpcAsyncTransport::StartGetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  if (request.id().empty()) {
    completion.Complete(core::Error::Validation(kGetPushConfigIdRequired));
    return;
  }
  StartRpc<lf::a2a::v1::TaskPushNotificationConfig>(
      request, options, completion, [](auto* stub, auto* context, auto* input, auto* output, auto done) {
        stub->GetTaskPushNotificationConfig(context, input, output, std::move(done));
      });
}
void GrpcAsyncTransport::StartListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) {
  StartRpc<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
      request, options, completion, [](auto* stub, auto* context, auto* input, auto* output, auto done) {
        stub->ListTaskPushNotificationConfigs(context, input, output, std::move(done));
      });
}
void GrpcAsyncTransport::StartDeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<void>& completion) {
  if (request.id().empty()) {
    completion.Complete(core::Error::Validation(kDeletePushConfigIdRequired));
    return;
  }
  StartRpc<google::protobuf::Empty>(
      request, options, completion,
      [](auto* stub, auto* context, auto* input, auto* output, auto done) {
        stub->DeleteTaskPushNotificationConfig(context, input, output, std::move(done));
      },
      DiscardResponse{});
}
}  // namespace
namespace grpc_internal {
std::shared_ptr<AsyncClientTransport> MakeAsyncTransport(ResolvedInterface resolved_interface,
                                                         std::shared_ptr<::grpc::Channel> channel,
                                                         std::chrono::milliseconds default_timeout) {
  return std::make_shared<GrpcAsyncTransport>(std::move(resolved_interface), std::move(channel), default_timeout);
}
}  // namespace grpc_internal
}  // namespace a2a::client
