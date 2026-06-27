// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/grpc_server_transport.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protocol_error_messages.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"
#include "a2a/core/version.h"

namespace a2a::server {
namespace {
::grpc::StatusCode RemoteProtocolStatusCode(const core::Error& error);

template <std::size_t MessageSize>
[[nodiscard]] ::grpc::Status InternalStatus(const std::array<char, MessageSize>& message) {
  return {::grpc::StatusCode::INTERNAL, core::protocol_error_messages::ToString(message)};
}

::grpc::StatusCode ToStatusCode(const core::Error& error) {
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return ::grpc::StatusCode::INVALID_ARGUMENT;
    case core::ErrorCode::kUnsupportedVersion:
      return ::grpc::StatusCode::UNIMPLEMENTED;
    case core::ErrorCode::kNetwork:
      return ::grpc::StatusCode::UNAVAILABLE;
    case core::ErrorCode::kRemoteProtocol:
      return RemoteProtocolStatusCode(error);
    case core::ErrorCode::kSerialization:
    case core::ErrorCode::kInternal:
      return ::grpc::StatusCode::INTERNAL;
  }
  return ::grpc::StatusCode::INTERNAL;
}

::grpc::StatusCode RemoteProtocolStatusCode(const core::Error& error) {
  const auto& protocol_code = error.protocol_code();
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kTaskNotFound) {
    return ::grpc::StatusCode::NOT_FOUND;
  }
  if (protocol_code.has_value() && (*protocol_code == core::protocol_codes::kPushNotificationNotSupported ||
                                    *protocol_code == core::protocol_codes::kUnsupportedOperation)) {
    return ::grpc::StatusCode::UNIMPLEMENTED;
  }
  return ::grpc::StatusCode::FAILED_PRECONDITION;
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

std::string ErrorInfoReason(const core::Error& error) {
  const auto& protocol_code = error.protocol_code();
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kTaskNotFound) {
    return "TASK_NOT_FOUND";
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kTaskNotCancelable) {
    return "TASK_NOT_CANCELABLE";
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kPushNotificationNotSupported) {
    return "PUSH_NOTIFICATION_NOT_SUPPORTED";
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kUnsupportedOperation) {
    return "UNSUPPORTED_OPERATION";
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kExtendedAgentCardNotConfigured) {
    return "EXTENDED_AGENT_CARD_NOT_CONFIGURED";
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kExtensionSupportRequired) {
    return "EXTENSION_SUPPORT_REQUIRED";
  }
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return "VALIDATION_ERROR";
    case core::ErrorCode::kUnsupportedVersion:
      return "VERSION_NOT_SUPPORTED";
    case core::ErrorCode::kNetwork:
      return "NETWORK_ERROR";
    case core::ErrorCode::kRemoteProtocol:
      return "REMOTE_PROTOCOL_ERROR";
    case core::ErrorCode::kSerialization:
      return "SERIALIZATION_ERROR";
    case core::ErrorCode::kInternal:
      return "INTERNAL_ERROR";
  }
  return "INTERNAL_ERROR";
}

int GrpcStatusCodeNumber(::grpc::StatusCode code) {
  constexpr int kStatusOk = 0;
  constexpr int kStatusCancelled = 1;
  constexpr int kStatusUnknown = 2;
  constexpr int kStatusInvalidArgument = 3;
  constexpr int kStatusDeadlineExceeded = 4;
  constexpr int kStatusNotFound = 5;
  constexpr int kStatusAlreadyExists = 6;
  constexpr int kStatusPermissionDenied = 7;
  constexpr int kStatusResourceExhausted = 8;
  constexpr int kStatusFailedPrecondition = 9;
  constexpr int kStatusAborted = 10;
  constexpr int kStatusOutOfRange = 11;
  constexpr int kStatusUnimplemented = 12;
  constexpr int kStatusInternal = 13;
  constexpr int kStatusUnavailable = 14;
  constexpr int kStatusDataLoss = 15;
  constexpr int kStatusUnauthenticated = 16;
  switch (code) {
    case ::grpc::StatusCode::OK:
      return kStatusOk;
    case ::grpc::StatusCode::CANCELLED:
      return kStatusCancelled;
    case ::grpc::StatusCode::UNKNOWN:
      return kStatusUnknown;
    case ::grpc::StatusCode::INVALID_ARGUMENT:
      return kStatusInvalidArgument;
    case ::grpc::StatusCode::DEADLINE_EXCEEDED:
      return kStatusDeadlineExceeded;
    case ::grpc::StatusCode::NOT_FOUND:
      return kStatusNotFound;
    case ::grpc::StatusCode::ALREADY_EXISTS:
      return kStatusAlreadyExists;
    case ::grpc::StatusCode::PERMISSION_DENIED:
      return kStatusPermissionDenied;
    case ::grpc::StatusCode::RESOURCE_EXHAUSTED:
      return kStatusResourceExhausted;
    case ::grpc::StatusCode::FAILED_PRECONDITION:
      return kStatusFailedPrecondition;
    case ::grpc::StatusCode::ABORTED:
      return kStatusAborted;
    case ::grpc::StatusCode::OUT_OF_RANGE:
      return kStatusOutOfRange;
    case ::grpc::StatusCode::UNIMPLEMENTED:
      return kStatusUnimplemented;
    case ::grpc::StatusCode::INTERNAL:
      return kStatusInternal;
    case ::grpc::StatusCode::UNAVAILABLE:
      return kStatusUnavailable;
    case ::grpc::StatusCode::DATA_LOSS:
      return kStatusDataLoss;
    case ::grpc::StatusCode::UNAUTHENTICATED:
      return kStatusUnauthenticated;
    case ::grpc::StatusCode::DO_NOT_USE:
      break;
  }
  return kStatusUnknown;
}

constexpr std::uint64_t kVarintContinuationBit = 0x80U;
constexpr std::uint64_t kVarintPayloadMask = 0x7FU;
constexpr std::uint32_t kVarintShiftBits = 7U;
constexpr int32_t kMaxListTasksPageSize = 100;
constexpr std::chrono::milliseconds kStreamCancellationPollInterval{50};

class StreamCancellationWatcher final {
 public:
  StreamCancellationWatcher(::grpc::ServerContext* context, ServerStreamSession* stream)
      : context_(context), stream_(stream) {
    if (context_ != nullptr && stream_ != nullptr && stream_->IsLive()) {
      worker_ = std::thread([this] { Watch(); });
    }
  }

  StreamCancellationWatcher(const StreamCancellationWatcher&) = delete;
  StreamCancellationWatcher& operator=(const StreamCancellationWatcher&) = delete;

  ~StreamCancellationWatcher() {
    stopped_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  void Watch() {
    while (!stopped_.load(std::memory_order_acquire)) {
      if (context_->IsCancelled()) {
        stream_->Cancel();
        return;
      }
      std::this_thread::sleep_for(kStreamCancellationPollInterval);
    }
  }

  ::grpc::ServerContext* context_ = nullptr;
  ServerStreamSession* stream_ = nullptr;
  std::atomic_bool stopped_ = false;
  std::thread worker_;
};

void AppendVarint(std::string& out, std::uint64_t value) {
  while (value >= kVarintContinuationBit) {
    out.push_back(static_cast<char>((value & kVarintPayloadMask) | kVarintContinuationBit));
    value >>= kVarintShiftBits;
  }
  out.push_back(static_cast<char>(value));
}

void AppendTag(std::string& out, std::uint32_t field_number, std::uint32_t wire_type) {
  AppendVarint(out, (static_cast<std::uint64_t>(field_number) << 3U) | wire_type);
}

void AppendLengthDelimited(std::string& out, std::uint32_t field_number, const std::string& value) {
  AppendTag(out, field_number, 2U);
  AppendVarint(out, static_cast<std::uint64_t>(value.size()));
  out.append(value);
}

std::string SerializeErrorInfo(const core::Error& error) {
  std::string message;
  AppendLengthDelimited(message, 1U, ErrorInfoReason(error));
  AppendLengthDelimited(message, 2U, "a2a-protocol.org");
  return message;
}

std::string SerializeAny(const std::string& type_url, const std::string& value) {
  std::string message;
  AppendLengthDelimited(message, 1U, type_url);
  AppendLengthDelimited(message, 2U, value);
  return message;
}

std::string SerializeGrpcStatusDetails(::grpc::StatusCode code, const core::Error& error) {
  std::string message;
  AppendTag(message, 1U, 0U);
  AppendVarint(message, static_cast<std::uint64_t>(GrpcStatusCodeNumber(code)));
  AppendLengthDelimited(message, 2U, std::string(error.message()));
  AppendLengthDelimited(message, 3U,
                        SerializeAny("type.googleapis.com/google.rpc.ErrorInfo", SerializeErrorInfo(error)));
  return message;
}

}  // namespace

GrpcServerTransport::GrpcServerTransport(Dispatcher* dispatcher, GrpcServerTransportOptions options)
    : dispatcher_(dispatcher), required_extensions_validator_(std::move(options.required_extensions)) {}

core::Result<RequestContext> GrpcServerTransport::BuildRequestContext(const ::grpc::ServerContext& context) const {
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
  const auto version_it = request_context.client_headers.find(std::string(GrpcServerTransport::kVersionMetadataKey));
  if (version_it == request_context.client_headers.end()) {
    return core::Error::UnsupportedVersion("Missing required A2A-Version header")
        .WithTransport(std::string(GrpcServerTransport::kTransportName));
  }
  if (version_it->second != core::Version::HeaderValue()) {
    return core::Error::UnsupportedVersion("Unsupported A2A-Version header value")
        .WithTransport(std::string(GrpcServerTransport::kTransportName));
  }

  const auto extensions = required_extensions_validator_.Validate(request_context.client_headers);
  if (!extensions.ok()) {
    return extensions.error().WithTransport(std::string(GrpcServerTransport::kTransportName));
  }
  return request_context;
}

::grpc::Status GrpcServerTransport::ToGrpcStatus(const core::Error& error, ::grpc::ServerContext* context) {
  const auto status_code = ToStatusCode(error);
  if (context != nullptr) {
    context->AddTrailingMetadata("a2a-error-code", ErrorCodeName(error.code()));
    context->AddTrailingMetadata("grpc-status-details-bin", SerializeGrpcStatusDetails(status_code, error));
    const auto& protocol_code = error.protocol_code();
    if (protocol_code.has_value()) {
      context->AddTrailingMetadata(std::string(GrpcServerTransport::kProtocolCodeMetadataKey), *protocol_code);
    }
  }

  return {status_code, std::string(error.message())};
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

  const auto dispatch = dispatcher_->Dispatch({.operation = DispatcherOperation::kSendMessage, .payload = *request},
                                              request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* payload = std::get_if<lf::a2a::v1::SendMessageResponse>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForSendMessage);
  }

  *response = *payload;
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::SendStreamingMessage(::grpc::ServerContext* context,
                                                         const lf::a2a::v1::SendMessageRequest* request,
                                                         ::grpc::ServerWriter<lf::a2a::v1::StreamResponse>* writer) {
  if (request == nullptr || writer == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and writer are required"};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  auto dispatch = dispatcher_->Dispatch({.operation = DispatcherOperation::kSendStreamingMessage, .payload = *request},
                                        request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* stream = std::get_if<std::unique_ptr<ServerStreamSession>>(&dispatch.value().payload());
  if (stream == nullptr || !(*stream)) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForSendStreamingMessage);
  }

  StreamCancellationWatcher cancellation_watcher(context, stream->get());
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
      (*stream)->Cancel();
      break;
    }
  }

  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::GetTask(::grpc::ServerContext* context, const lf::a2a::v1::GetTaskRequest* request,
                                            lf::a2a::v1::Task* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kGetTask, .payload = *request}, request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* payload = std::get_if<lf::a2a::v1::Task>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForGetTask);
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

  const auto dispatch = dispatcher_->Dispatch({.operation = DispatcherOperation::kCancelTask, .payload = *request},
                                              request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* payload = std::get_if<lf::a2a::v1::Task>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForCancelTask);
  }

  *response = *payload;
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::ListTasks(::grpc::ServerContext* context,
                                              const lf::a2a::v1::ListTasksRequest* request,
                                              lf::a2a::v1::ListTasksResponse* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  ListTasksRequest list_request;
  if (request->has_page_size()) {
    const int32_t page_size = request->page_size();
    if (page_size <= 0 || page_size > kMaxListTasksPageSize) {
      return ToGrpcStatus(core::Error::Validation("ListTasksRequest.page_size must be between 1 and 100"), context);
    }
    list_request.page_size = static_cast<std::size_t>(page_size);
  }
  list_request.page_token = request->page_token();
  list_request.context_id = request->context_id();
  if (request->status() != lf::a2a::v1::TASK_STATE_UNSPECIFIED) {
    list_request.status_filter = request->status();
  }
  if (request->has_history_length()) {
    if (request->history_length() < 0) {
      return ToGrpcStatus(core::Error::Validation("ListTasksRequest.history_length must be non-negative"), context);
    }
    list_request.history_length = static_cast<std::size_t>(request->history_length());
  }
  if (request->has_include_artifacts()) {
    list_request.include_artifacts = request->include_artifacts();
  }
  if (request->has_status_timestamp_after()) {
    list_request.status_timestamp_after = request->status_timestamp_after();
  }

  const auto dispatch = dispatcher_->Dispatch({.operation = DispatcherOperation::kListTasks, .payload = list_request},
                                              request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* payload = std::get_if<ListTasksResponse>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForListTasks);
  }

  for (const auto& task : payload->tasks) {
    *response->add_tasks() = task;
  }
  response->set_page_size(static_cast<int32_t>(payload->page_size));
  response->set_total_size(static_cast<int32_t>(payload->total_size));
  response->set_next_page_token(payload->next_page_token);
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::SubscribeToTask(::grpc::ServerContext* context,
                                                    const lf::a2a::v1::SubscribeToTaskRequest* request,
                                                    ::grpc::ServerWriter<lf::a2a::v1::StreamResponse>* writer) {
  if (request == nullptr || writer == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and writer are required"};
  }

  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }

  lf::a2a::v1::GetTaskRequest get_task_request;
  get_task_request.set_id(request->id());
  const auto dispatch = dispatcher_->Dispatch(
      {.operation = DispatcherOperation::kSubscribeTask, .payload = get_task_request}, request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }

  const auto* stream = std::get_if<std::unique_ptr<ServerStreamSession>>(&dispatch.value().payload());
  if (stream == nullptr || *stream == nullptr) {
    return InternalStatus(core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForSubscribeToTask);
  }

  StreamCancellationWatcher cancellation_watcher(context, stream->get());
  while (!context->IsCancelled()) {
    const auto next = (*stream)->Next();
    if (!next.ok()) {
      return ToGrpcStatus(next.error(), context);
    }
    const auto& maybe_event = next.value();
    if (!maybe_event.has_value()) {
      break;
    }
    if (!writer->Write(maybe_event.value())) {
      (*stream)->Cancel();
      return {::grpc::StatusCode::INTERNAL, "Failed to write stream event"};
    }
  }

  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::CreateTaskPushNotificationConfig(
    ::grpc::ServerContext* context, const lf::a2a::v1::TaskPushNotificationConfig* request,
    lf::a2a::v1::TaskPushNotificationConfig* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kCreateTaskPushNotificationConfig, .payload = *request},
                            request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }
  const auto* payload = std::get_if<lf::a2a::v1::TaskPushNotificationConfig>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(
        core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForCreateTaskPushNotificationConfig);
  }
  *response = *payload;
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::GetTaskPushNotificationConfig(
    ::grpc::ServerContext* context, const lf::a2a::v1::GetTaskPushNotificationConfigRequest* request,
    lf::a2a::v1::TaskPushNotificationConfig* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch = dispatcher_->Dispatch(
      {.operation = DispatcherOperation::kGetTaskPushNotificationConfig, .payload = *request}, request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }
  const auto* payload = std::get_if<lf::a2a::v1::TaskPushNotificationConfig>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(
        core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForGetTaskPushNotificationConfig);
  }
  *response = *payload;
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::ListTaskPushNotificationConfigs(
    ::grpc::ServerContext* context, const lf::a2a::v1::ListTaskPushNotificationConfigsRequest* request,
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kListTaskPushNotificationConfigs, .payload = *request},
                            request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }
  const auto* payload = std::get_if<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(&dispatch.value().payload());
  if (payload == nullptr) {
    return InternalStatus(
        core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForListTaskPushNotificationConfigs);
  }
  *response = *payload;
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::DeleteTaskPushNotificationConfig(
    ::grpc::ServerContext* context, const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest* request,
    google::protobuf::Empty* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }
  auto request_context = BuildRequestContext(*context);
  if (!request_context.ok()) {
    return ToGrpcStatus(request_context.error(), context);
  }
  const auto dispatch =
      dispatcher_->Dispatch({.operation = DispatcherOperation::kDeleteTaskPushNotificationConfig, .payload = *request},
                            request_context.value());
  if (!dispatch.ok()) {
    return ToGrpcStatus(dispatch.error(), context);
  }
  return ::grpc::Status::OK;
}

::grpc::Status GrpcServerTransport::GetExtendedAgentCard(::grpc::ServerContext* context,
                                                         const lf::a2a::v1::GetExtendedAgentCardRequest* request,
                                                         lf::a2a::v1::AgentCard* response) {
  if (request == nullptr || response == nullptr) {
    return {::grpc::StatusCode::INVALID_ARGUMENT, "Request and response are required"};
  }
  (void)request;
  (void)context;

  response->set_name("A2A C++ SDK Agent");
  response->set_description("Default agent card for compatibility checks");
  response->set_version(std::string(core::Version::kAgentCardVersion));
  response->add_default_input_modes("text/plain");
  response->add_default_output_modes("text/plain");
  auto* capabilities = response->mutable_capabilities();
  capabilities->set_push_notifications(false);
  capabilities->set_streaming(true);
  for (const auto& required_extension : required_extensions_validator_.required_extensions()) {
    auto* extension = capabilities->add_extensions();
    extension->set_uri(required_extension);
    extension->set_required(true);
  }

  return ::grpc::Status::OK;
}

}  // namespace a2a::server
