// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/grpc_server_transport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_error_messages.h"
#include "server/transport/grpc_dispatch.h"
#include "stream_cancellation_watcher.h"

namespace a2a::server {
namespace {
constexpr auto kDispatcherMissing = "Server dispatcher is not configured";
constexpr auto kUnaryArgumentsMissing = "Request and response are required";
constexpr auto kStreamArgumentsMissing = "Request and writer are required";
constexpr auto kStreamWriteFailed = "Failed to write stream event";
constexpr auto kCardPayloadMismatch = "GetExtendedAgentCard dispatch returned an unexpected payload";
template <std::size_t MessageSize>
[[nodiscard]] ::grpc::Status InternalStatus(const std::array<char, MessageSize>& message) {
  return {::grpc::StatusCode::INTERNAL, core::protocol_error_messages::ToString(message)};
}
}  // namespace

GrpcServerTransport::GrpcServerTransport(Dispatcher* dispatcher, GrpcServerTransportOptions options)
    : dispatcher_(dispatcher), required_extensions_validator_(std::move(options.required_extensions)) {}

core::Result<GrpcServerTransport::ValidatedRequestContext> GrpcServerTransport::BuildRequestContext(
    const ::grpc::ServerContext& context) const {
  if (dispatcher_ == nullptr) {
    return core::Error::Internal(kDispatcherMissing);
  }

  auto validated = BuildGrpcRequestContext(context, required_extensions_validator_);
  if (!validated.ok()) {
    return validated.error();
  }
  return ValidatedRequestContext{.request_context = std::move(validated.value().request_context),
                                 .activated_extensions = std::move(validated.value().activated_extensions)};
}

::grpc::Status GrpcServerTransport::ToGrpcStatus(const core::Error& error, ::grpc::ServerContext* context,
                                                 const std::vector<std::string>& activated_extensions) {
  return GrpcErrorStatus(error, context, activated_extensions);
}

::grpc::Status GrpcServerTransport::SendMessage(::grpc::ServerContext* context,
                                                const lf::a2a::v1::SendMessageRequest* request,
                                                lf::a2a::v1::SendMessageResponse* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  const auto dispatch = dispatcher_->Dispatch({.operation = DispatcherOperation::kSendMessage, .payload = *request},
                                              request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }

  const auto* payload = std::get_if<lf::a2a::v1::SendMessageResponse>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForSendMessage);
  }

  *response = *payload;
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::SendStreamingMessage(::grpc::ServerContext* context,
                                                         const lf::a2a::v1::SendMessageRequest* request,
                                                         ::grpc::ServerWriter<lf::a2a::v1::StreamResponse>* writer) {
  if (request == nullptr || writer == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kStreamArgumentsMissing};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  auto dispatch = dispatcher_->Dispatch({.operation = DispatcherOperation::kSendStreamingMessage, .payload = *request},
                                        request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }

  const auto* stream = std::get_if<std::unique_ptr<ServerStreamSession>>(&dispatch.value().payload());
  if (stream == nullptr || !(*stream)) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForSendStreamingMessage);
  }

  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  const bool supports_timed_next = (*stream)->SupportsTimedNext();
  std::optional<StreamCancellationWatcher> cancellation_watcher;
  if (!supports_timed_next) {
    cancellation_watcher.emplace(context, stream->get());
  }
  while (!context->IsCancelled()) {
    const auto next = supports_timed_next ? (*stream)->NextFor(kStreamCancellationPollInterval) : (*stream)->Next();
    if (!next.ok()) {
      return ToGrpcStatus(next.error(), context, request_context.value().activated_extensions);
    }
    const auto& event = next.value();
    if (!event.has_value() && (*stream)->IsLive()) {
      continue;
    }
    if (!event.has_value()) {
      break;
    }
    if (!writer->Write(*event)) {
      (*stream)->Cancel();
      return ::grpc::Status::OK;
    }
  }

  if (supports_timed_next && context->IsCancelled()) {
    (*stream)->Cancel();
  }

  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::GetTask(::grpc::ServerContext* context, const lf::a2a::v1::GetTaskRequest* request,
                                            lf::a2a::v1::Task* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  const auto dispatch = dispatcher_->Dispatch({.operation = DispatcherOperation::kGetTask, .payload = *request},
                                              request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }

  const auto* payload = std::get_if<lf::a2a::v1::Task>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForGetTask);
  }

  *response = *payload;
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::CancelTask(::grpc::ServerContext* context,
                                               const lf::a2a::v1::CancelTaskRequest* request,
                                               lf::a2a::v1::Task* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  const auto dispatch = dispatcher_->Dispatch({.operation = DispatcherOperation::kCancelTask, .payload = *request},
                                              request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }

  const auto* payload = std::get_if<lf::a2a::v1::Task>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForCancelTask);
  }

  *response = *payload;
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::ListTasks(::grpc::ServerContext* context,
                                              const lf::a2a::v1::ListTasksRequest* request,
                                              lf::a2a::v1::ListTasksResponse* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  const auto list_request = ParseGrpcListTasksRequest(*request);
  if (!list_request.ok()) {
    return ToGrpcStatus(list_request.error(), context, request_context.value().activated_extensions);
  }

  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kListTasks, .payload = list_request.value()},
                            request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }

  const auto* payload = std::get_if<ListTasksResponse>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForListTasks);
  }

  WriteGrpcListTasksResponse(*payload, *response);
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::SubscribeToTask(::grpc::ServerContext* context,
                                                    const lf::a2a::v1::SubscribeToTaskRequest* request,
                                                    ::grpc::ServerWriter<lf::a2a::v1::StreamResponse>* writer) {
  if (request == nullptr || writer == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kStreamArgumentsMissing};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  lf::a2a::v1::GetTaskRequest get_task_request;
  get_task_request.set_id(request->id());
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kSubscribeTask, .payload = get_task_request},
                            request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }

  const auto* stream = std::get_if<std::unique_ptr<ServerStreamSession>>(&dispatch.value().payload());
  if (stream == nullptr || *stream == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForSubscribeToTask);
  }

  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  const bool supports_timed_next = (*stream)->SupportsTimedNext();
  std::optional<StreamCancellationWatcher> cancellation_watcher;
  if (!supports_timed_next) {
    cancellation_watcher.emplace(context, stream->get());
  }
  while (!context->IsCancelled()) {
    const auto next = supports_timed_next ? (*stream)->NextFor(kStreamCancellationPollInterval) : (*stream)->Next();
    if (!next.ok()) {
      return ToGrpcStatus(next.error(), context, request_context.value().activated_extensions);
    }
    const auto& maybe_event = next.value();
    if (!maybe_event.has_value() && (*stream)->IsLive()) {
      continue;
    }
    if (!maybe_event.has_value()) {
      break;
    }
    if (!writer->Write(maybe_event.value())) {
      (*stream)->Cancel();
      return {::grpc::StatusCode::INTERNAL, kStreamWriteFailed};
    }
  }

  if (supports_timed_next && context->IsCancelled()) {
    (*stream)->Cancel();
  }

  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::CreateTaskPushNotificationConfig(
    ::grpc::ServerContext* context, const lf::a2a::v1::TaskPushNotificationConfig* request,
    lf::a2a::v1::TaskPushNotificationConfig* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kCreateTaskPushNotificationConfig, .payload = *request},
                            request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }
  const auto* payload = std::get_if<lf::a2a::v1::TaskPushNotificationConfig>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(
        core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForCreateTaskPushNotificationConfig);
  }
  *response = *payload;
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::GetTaskPushNotificationConfig(
    ::grpc::ServerContext* context, const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
    lf::a2a::v1::TaskPushNotificationConfig* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kGetTaskPushNotificationConfig, .payload = *request},
                            request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }
  const auto* payload = std::get_if<lf::a2a::v1::TaskPushNotificationConfig>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(
        core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForGetTaskPushNotificationConfig);
  }
  *response = *payload;
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::ListTaskPushNotificationConfigs(
    ::grpc::ServerContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kListTaskPushNotificationConfigs, .payload = *request},
                            request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }
  const auto* payload = std::get_if<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(
        core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForListTaskPushNotificationConfigs);
  }
  *response = *payload;
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::DeleteTaskPushNotificationConfig(
    ::grpc::ServerContext* context, const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
    google::protobuf::Empty* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kDeleteTaskPushNotificationConfig, .payload = *request},
                            request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::GetExtendedAgentCard(::grpc::ServerContext* context,
                                                         const lf::a2a::v1::GetExtendedAgentCardRequest* request,
                                                         lf::a2a::v1::AgentCard* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, kUnaryArgumentsMissing};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kGetExtendedAgentCard, .payload = *request},
                            request_context.value().request_context);
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context, request_context.value().activated_extensions);
  }
  const auto* payload = std::get_if<lf::a2a::v1::AgentCard>(&dispatch.value().payload());
  if (payload == nullptr) {
    return {::grpc::StatusCode::INTERNAL, kCardPayloadMismatch};
  }

  *response = *payload;
  AddGrpcActivatedExtensions(context, request_context.value().activated_extensions);
  return ::grpc::Status::OK;
}

}  // namespace a2a::server
