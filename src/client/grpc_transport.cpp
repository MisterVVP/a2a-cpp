// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/grpc_transport.h"

#include <grpcpp/create_channel.h>

#include <cctype>
#include <memory>
#include <ranges>
#include <string>
#include <thread>

#include "a2a/client/auth.h"
#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/version.h"
#include "a2a/v1/a2a.grpc.pb.h"

namespace a2a::client {
namespace {

class StubStreamReader final : public GrpcTransport::StreamReader {
 public:
  explicit StubStreamReader(std::unique_ptr<::grpc::ClientReaderInterface<lf::a2a::v1::StreamResponse>> reader)
      : reader_(std::move(reader)) {}

  bool Read(lf::a2a::v1::StreamResponse* response) override { return reader_->Read(response); }

  [[nodiscard]] ::grpc::Status Finish() override { return reader_->Finish(); }

 private:
  std::unique_ptr<::grpc::ClientReaderInterface<lf::a2a::v1::StreamResponse>> reader_;
};

class StubRpcClient final : public GrpcTransport::RpcClient {
 public:
  explicit StubRpcClient(std::unique_ptr<lf::a2a::v1::A2AService::StubInterface> stub) : stub_(std::move(stub)) {}

  [[nodiscard]] ::grpc::Status SendMessage(::grpc::ClientContext* context,
                                           const lf::a2a::v1::SendMessageRequest& request,
                                           lf::a2a::v1::SendMessageResponse* response) override {
    return stub_->SendMessage(context, request, response);
  }

  [[nodiscard]] std::unique_ptr<GrpcTransport::StreamReader> SendStreamingMessage(
      ::grpc::ClientContext* context, const lf::a2a::v1::SendMessageRequest& request) override {
    return std::make_unique<StubStreamReader>(stub_->SendStreamingMessage(context, request));
  }

  [[nodiscard]] ::grpc::Status GetTask(::grpc::ClientContext* context, const lf::a2a::v1::GetTaskRequest& request,
                                       lf::a2a::v1::Task* response) override {
    return stub_->GetTask(context, request, response);
  }

  [[nodiscard]] ::grpc::Status ListTasks(::grpc::ClientContext* context, const lf::a2a::v1::ListTasksRequest& request,
                                         lf::a2a::v1::ListTasksResponse* response) override {
    return stub_->ListTasks(context, request, response);
  }

  [[nodiscard]] std::unique_ptr<GrpcTransport::StreamReader> SubscribeToTask(
      ::grpc::ClientContext* context, const lf::a2a::v1::SubscribeToTaskRequest& request) override {
    return std::make_unique<StubStreamReader>(stub_->SubscribeToTask(context, request));
  }

  [[nodiscard]] ::grpc::Status CancelTask(::grpc::ClientContext* context, const lf::a2a::v1::CancelTaskRequest& request,
                                          lf::a2a::v1::Task* response) override {
    return stub_->CancelTask(context, request, response);
  }

  [[nodiscard]] ::grpc::Status CreateTaskPushNotificationConfig(
      ::grpc::ClientContext* context, const lf::a2a::v1::TaskPushNotificationConfig& request,
      lf::a2a::v1::TaskPushNotificationConfig* response) override {
    return stub_->CreateTaskPushNotificationConfig(context, request, response);
  }

  [[nodiscard]] ::grpc::Status GetTaskPushNotificationConfig(
      ::grpc::ClientContext* context, const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      lf::a2a::v1::TaskPushNotificationConfig* response) override {
    return stub_->GetTaskPushNotificationConfig(context, request, response);
  }

  [[nodiscard]] ::grpc::Status ListTaskPushNotificationConfigs(
      ::grpc::ClientContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) override {
    return stub_->ListTaskPushNotificationConfigs(context, request, response);
  }

  [[nodiscard]] ::grpc::Status DeleteTaskPushNotificationConfig(
      ::grpc::ClientContext* context, const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      google::protobuf::Empty* response) override {
    return stub_->DeleteTaskPushNotificationConfig(context, request, response);
  }

 private:
  std::unique_ptr<lf::a2a::v1::A2AService::StubInterface> stub_;
};

}  // namespace

GrpcTransport::GrpcTransport(ResolvedInterface resolved_interface, std::shared_ptr<::grpc::Channel> channel,
                             std::chrono::milliseconds default_timeout)
    : GrpcTransport(std::move(resolved_interface),
                    std::make_unique<StubRpcClient>(lf::a2a::v1::A2AService::NewStub(std::move(channel))),
                    default_timeout) {}

GrpcTransport::GrpcTransport(ResolvedInterface resolved_interface, std::unique_ptr<RpcClient> rpc_client,
                             std::chrono::milliseconds default_timeout)
    : resolved_interface_(std::move(resolved_interface)),
      rpc_client_(std::move(rpc_client)),
      default_timeout_(default_timeout) {}

core::Result<std::unique_ptr<::grpc::ClientContext>> GrpcTransport::BuildContext(const CallOptions& options) const {
  if (resolved_interface_.transport != PreferredTransport::kGrpc) {
    return core::Error::Validation("GrpcTransport requires a gRPC interface");
  }
  if (resolved_interface_.url.empty()) {
    return core::Error::Validation("Resolved gRPC interface URL is required");
  }
  if (rpc_client_ == nullptr) {
    return core::Error::Internal("gRPC stub is not configured");
  }

  auto context = std::make_unique<::grpc::ClientContext>();
  const auto timeout = options.timeout.value_or(default_timeout_);
  context->set_deadline(std::chrono::system_clock::now() + timeout);

  HeaderMap headers = options.headers;
  headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();

  if (!options.extensions.empty()) {
    headers[std::string(core::Extensions::kHeaderName)] = core::Extensions::Format(options.extensions);
  }
  if (options.auth_hook) {
    options.auth_hook(headers);
  }
  if (options.credential_provider != nullptr) {
    const auto apply = ApplyCredentialProvider(*options.credential_provider, options.auth_context, &headers);
    if (!apply.ok()) {
      return apply.error();
    }
  }

  for (const auto& [name, value] : headers) {
    std::string lowered_name = name;
    std::ranges::transform(lowered_name, lowered_name.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    context->AddMetadata(lowered_name, value);
  }
  return context;
}

core::Error GrpcTransport::BuildGrpcError(const ::grpc::Status& status) {
  core::Error error = core::Error::RemoteProtocol(status.error_message()).WithTransport("grpc");
  error = error.WithProtocolCode(std::to_string(static_cast<int>(status.error_code())));
  return error;
}

core::Result<lf::a2a::v1::SendMessageResponse> GrpcTransport::SendMessage(
    const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) {
  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }
  auto context = std::move(context_result.value());
  lf::a2a::v1::SendMessageResponse response;
  const auto status = rpc_client_->SendMessage(context.get(), request, &response);
  if (!status.ok()) {
    return BuildGrpcError(status);
  }
  return response;
}

core::Result<lf::a2a::v1::Task> GrpcTransport::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                       const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskRequest.id is required");
  }

  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }
  auto context = std::move(context_result.value());
  lf::a2a::v1::Task response;
  const auto status = rpc_client_->GetTask(context.get(), request, &response);
  if (!status.ok()) {
    return BuildGrpcError(status);
  }
  return response;
}

core::Result<ListTasksResponse> GrpcTransport::ListTasks(const ListTasksRequest& request, const CallOptions& options) {
  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }

  lf::a2a::v1::ListTasksRequest grpc_request;
  if (request.page_size > 0) {
    grpc_request.set_page_size(static_cast<int32_t>(request.page_size));
  }
  grpc_request.set_page_token(request.page_token);

  auto context = std::move(context_result.value());
  lf::a2a::v1::ListTasksResponse response;
  const auto status = rpc_client_->ListTasks(context.get(), grpc_request, &response);
  if (!status.ok()) {
    return BuildGrpcError(status);
  }

  ListTasksResponse result;
  result.tasks.reserve(static_cast<std::size_t>(response.tasks_size()));
  for (const auto& task : response.tasks()) {
    result.tasks.push_back(task);
  }
  result.next_page_token = response.next_page_token();
  return result;
}

core::Result<lf::a2a::v1::Task> GrpcTransport::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                          const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("CancelTaskRequest.id is required");
  }

  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }
  auto context = std::move(context_result.value());
  lf::a2a::v1::Task response;
  const auto status = rpc_client_->CancelTask(context.get(), request, &response);
  if (!status.ok()) {
    return BuildGrpcError(status);
  }
  return response;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> GrpcTransport::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }
  auto context = std::move(context_result.value());
  lf::a2a::v1::TaskPushNotificationConfig response;
  const auto status = rpc_client_->CreateTaskPushNotificationConfig(context.get(), request, &response);
  if (!status.ok()) {
    return BuildGrpcError(status);
  }
  return response;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> GrpcTransport::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskPushNotificationConfigRequest.id is required");
  }
  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }
  auto context = std::move(context_result.value());
  lf::a2a::v1::TaskPushNotificationConfig response;
  const auto status = rpc_client_->GetTaskPushNotificationConfig(context.get(), request, &response);
  if (!status.ok()) {
    return BuildGrpcError(status);
  }
  return response;
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> GrpcTransport::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) {
  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }
  auto context = std::move(context_result.value());
  lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
  const auto status = rpc_client_->ListTaskPushNotificationConfigs(context.get(), request, &response);
  if (!status.ok()) {
    return BuildGrpcError(status);
  }
  return response;
}

core::Result<void> GrpcTransport::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("DeleteTaskPushNotificationConfigRequest.id is required");
  }

  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }
  auto context = std::move(context_result.value());

  google::protobuf::Empty response;
  const auto status = rpc_client_->DeleteTaskPushNotificationConfig(context.get(), request, &response);
  if (!status.ok()) {
    return BuildGrpcError(status);
  }
  return {};
}

core::Result<std::unique_ptr<StreamHandle>> GrpcTransport::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) {
  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }

  auto state = std::make_shared<StreamHandle::State>();
  auto rpc_client = rpc_client_;
  auto context = std::shared_ptr<::grpc::ClientContext>(std::move(context_result.value()));
  {
    std::lock_guard lock(state->cancellation_mutex);
    state->cancel_callback = [rpc_client, weak_context = std::weak_ptr<::grpc::ClientContext>(context)] {
      if (const auto context = weak_context.lock(); context != nullptr) {
        rpc_client->CancelStream(context.get());
      }
    };
  }
  auto worker =
      StreamHandle::WorkerThread([rpc_client, state, request, &observer, context = std::move(context)]() mutable {
        auto reader = rpc_client->SendStreamingMessage(context.get(), request);
        if (reader == nullptr) {
          observer.OnError(core::Error::Internal("Failed to create gRPC stream reader"));
          state->active.store(false);
          return;
        }

        lf::a2a::v1::StreamResponse event;
        while (!state->cancel_requested.load() && reader->Read(&event)) {
          observer.OnEvent(event);
        }

        const auto status = reader->Finish();
        if (state->cancel_requested.load()) {
          state->active.store(false);
          return;
        }

        if (!status.ok()) {
          observer.OnError(GrpcTransport::BuildGrpcError(status));
          state->active.store(false);
          return;
        }

        observer.OnCompleted();
        state->active.store(false);
      });

  return std::unique_ptr<StreamHandle>(new StreamHandle(state, std::move(worker)));
}

core::Result<std::unique_ptr<StreamHandle>> GrpcTransport::SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                         StreamObserver& observer,
                                                                         const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskRequest.id is required");
  }

  auto context_result = BuildContext(options);
  if (!context_result.ok()) {
    return context_result.error();
  }

  lf::a2a::v1::SubscribeToTaskRequest subscribe_request;
  subscribe_request.set_id(request.id());

  auto state = std::make_shared<StreamHandle::State>();
  auto rpc_client = rpc_client_;
  auto context = std::shared_ptr<::grpc::ClientContext>(std::move(context_result.value()));
  {
    std::lock_guard lock(state->cancellation_mutex);
    state->cancel_callback = [rpc_client, weak_context = std::weak_ptr<::grpc::ClientContext>(context)] {
      if (const auto context = weak_context.lock(); context != nullptr) {
        rpc_client->CancelStream(context.get());
      }
    };
  }
  auto worker = StreamHandle::WorkerThread(
      [rpc_client, state, subscribe_request, &observer, context = std::move(context)]() mutable {
        auto reader = rpc_client->SubscribeToTask(context.get(), subscribe_request);
        if (reader == nullptr) {
          observer.OnError(core::Error::Internal("Failed to create gRPC subscribe stream reader"));
          state->active.store(false);
          return;
        }

        lf::a2a::v1::StreamResponse event;
        while (!state->cancel_requested.load() && reader->Read(&event)) {
          observer.OnEvent(event);
        }

        const auto status = reader->Finish();
        if (state->cancel_requested.load()) {
          state->active.store(false);
          return;
        }

        if (!status.ok()) {
          observer.OnError(GrpcTransport::BuildGrpcError(status));
          state->active.store(false);
          return;
        }

        observer.OnCompleted();
        state->active.store(false);
      });

  return std::unique_ptr<StreamHandle>(new StreamHandle(state, std::move(worker)));
}

}  // namespace a2a::client
