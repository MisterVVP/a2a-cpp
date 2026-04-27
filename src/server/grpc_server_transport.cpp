#include "a2a/server/grpc_server_transport.h"

#include <optional>
#include <string>
#include <utility>

#include "a2a/core/error.h"

namespace a2a::server {
namespace {

::grpc::StatusCode ToStatusCode(const core::Error& error) {
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return ::grpc::StatusCode::INVALID_ARGUMENT;
    case core::ErrorCode::kUnsupportedVersion:
      return ::grpc::StatusCode::FAILED_PRECONDITION;
    case core::ErrorCode::kNetwork:
      return ::grpc::StatusCode::UNAVAILABLE;
    case core::ErrorCode::kRemoteProtocol:
      return ::grpc::StatusCode::FAILED_PRECONDITION;
    case core::ErrorCode::kSerialization:
    case core::ErrorCode::kInternal:
      return ::grpc::StatusCode::INTERNAL;
  }
  return ::grpc::StatusCode::INTERNAL;
}

std::string ErrorCodeName(core::ErrorCode code) {
  switch (code) {
    case core::ErrorCode::kValidation:
      return "validation";
    case core::ErrorCode::kUnsupportedVersion:
      return "unsupported_version";
    case core::ErrorCode::kNetwork:
      return "network";
    case core::ErrorCode::kRemoteProtocol:
      return "remote_protocol";
    case core::ErrorCode::kSerialization:
      return "serialization";
    case core::ErrorCode::kInternal:
      return "internal";
  }
  return "internal";
}

}  // namespace

GrpcServerTransport::GrpcServerTransport(Dispatcher* dispatcher) : dispatcher_(dispatcher) {}

core::Result<RequestContext> GrpcServerTransport::BuildRequestContext(
    const ::grpc::ServerContext& context) const {
  if (dispatcher_ == nullptr) {
    return core::Error::Internal("Server dispatcher is not configured");
  }

  RequestContext request_context;
  request_context.remote_address = context.peer();

  for (const auto& [key_ref, value_ref] : context.client_metadata()) {
    const std::string key(key_ref.data(), key_ref.length());
    const std::string value(value_ref.data(), value_ref.length());
    request_context.client_headers[key] = value;
  }
  request_context.auth_metadata = ExtractAuthMetadata(request_context.client_headers);
  return request_context;
}

::grpc::Status GrpcServerTransport::ToGrpcStatus(const core::Error& error,
                                                 ::grpc::ServerContext* context) {
  if (context != nullptr) {
    context->AddTrailingMetadata("a2a-error-code", ErrorCodeName(error.code()));
    const auto& protocol_code = error.protocol_code();
    if (protocol_code.has_value()) {
      context->AddTrailingMetadata("a2a-protocol-code", *protocol_code);
    }
  }

  return {ToStatusCode(error), std::string(error.message())};
}

::grpc::Status GrpcServerTransport::SendMessage(::grpc::ServerContext* context,
                                                const lf::a2a::v1::SendMessageRequest* request,
                                                lf::a2a::v1::SendMessageResponse* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kSendMessage, .payload = *request},
                            request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* payload = std::get_if<lf::a2a::v1::SendMessageResponse>(&dispatch.value().payload());
  if (payload == nullptr) {
    return {::grpc::StatusCode::INTERNAL, "Unexpected dispatch payload type for SendMessage"};
  }

  *response = *payload;
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::SendStreamingMessage(
    ::grpc::ServerContext* context, const lf::a2a::v1::SendMessageRequest* request,
    ::grpc::ServerWriter<lf::a2a::v1::StreamResponse>* writer) {
  if (request == nullptr || writer == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and writer are required"};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  auto dispatch = dispatcher_->Dispatch(
      {.operation = DispatcherOperation::kSendStreamingMessage, .payload = *request},
      request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  auto* stream = std::get_if<std::unique_ptr<ServerStreamSession>>(&dispatch.value().payload());
  if (stream == nullptr || !(*stream)) {
    return {::grpc::StatusCode::INTERNAL,
            "Unexpected dispatch payload type for SendStreamingMessage"};
  }

  while (!context->IsCancelled()) {
    const auto next = (*stream)->Next();
    if (!next.ok()) {
      return ToGrpcStatus(next.error(), context);
    }
    const auto& event = next.value();
    if (!event.has_value()) {
      break;
    }
    if (!writer->Write(*event)) {
      break;
    }
  }

  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::GetTask(::grpc::ServerContext* context,
                                            const lf::a2a::v1::GetTaskRequest* request,
                                            lf::a2a::v1::Task* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  const auto dispatch = dispatcher_->Dispatch(
      {.operation = DispatcherOperation::kGetTask, .payload = *request}, request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* payload = std::get_if<lf::a2a::v1::Task>(&dispatch.value().payload());
  if (payload == nullptr) {
    return {::grpc::StatusCode::INTERNAL, "Unexpected dispatch payload type for GetTask"};
  }

  *response = *payload;
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::CancelTask(::grpc::ServerContext* context,
                                               const lf::a2a::v1::CancelTaskRequest* request,
                                               lf::a2a::v1::Task* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kCancelTask, .payload = *request},
                            request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* payload = std::get_if<lf::a2a::v1::Task>(&dispatch.value().payload());
  if (payload == nullptr) {
    return {::grpc::StatusCode::INTERNAL, "Unexpected dispatch payload type for CancelTask"};
  }

  *response = *payload;
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::SetTaskPushNotificationConfig(
    ::grpc::ServerContext* context, const lf::a2a::v1::TaskPushNotificationConfig* request,
    lf::a2a::v1::TaskPushNotificationConfig* response) {
  (void)context;
  (void)request;
  (void)response;
  return {::grpc::StatusCode::UNIMPLEMENTED, "Not implemented"};
}

::grpc::Status GrpcServerTransport::GetTaskPushNotificationConfig(
    ::grpc::ServerContext* context,
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
    lf::a2a::v1::TaskPushNotificationConfig* response) {
  (void)context;
  (void)request;
  (void)response;
  return {::grpc::StatusCode::UNIMPLEMENTED, "Not implemented"};
}

::grpc::Status GrpcServerTransport::ListTaskPushNotificationConfigs(
    ::grpc::ServerContext* context,
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) {
  (void)context;
  (void)request;
  (void)response;
  return {::grpc::StatusCode::UNIMPLEMENTED, "Not implemented"};
}

::grpc::Status GrpcServerTransport::DeleteTaskPushNotificationConfig(
    ::grpc::ServerContext* context,
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
    google::protobuf::Empty* response) {
  (void)context;
  (void)request;
  (void)response;
  return {::grpc::StatusCode::UNIMPLEMENTED, "Not implemented"};
}

}  // namespace a2a::server
