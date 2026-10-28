// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/transport/grpc_dispatch.h"

#include <chrono>
#include <cstdint>
#include <utility>

#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/version.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/version_header_validator.h"
namespace a2a::server {
namespace {
constexpr auto kValidationCode = "validation";
constexpr auto kUnsupportedVersionCode = "unsupported_version";
constexpr auto kNetworkCode = "network";
constexpr auto kRemoteProtocolCode = "remote_protocol";
constexpr auto kSerializationCode = "serialization";
constexpr auto kInternalCode = "internal";
constexpr auto kTaskNotFoundReason = "TASK_NOT_FOUND";
constexpr auto kTaskNotCancelableReason = "TASK_NOT_CANCELABLE";
constexpr auto kPushNotSupportedReason = "PUSH_NOTIFICATION_NOT_SUPPORTED";
constexpr auto kUnsupportedOperationReason = "UNSUPPORTED_OPERATION";
constexpr auto kExtendedCardNotConfiguredReason = "EXTENDED_AGENT_CARD_NOT_CONFIGURED";
constexpr auto kExtensionRequiredReason = "EXTENSION_SUPPORT_REQUIRED";
constexpr auto kValidationReason = "VALIDATION_ERROR";
constexpr auto kVersionReason = "VERSION_NOT_SUPPORTED";
constexpr auto kNetworkReason = "NETWORK_ERROR";
constexpr auto kRemoteProtocolReason = "REMOTE_PROTOCOL_ERROR";
constexpr auto kSerializationReason = "SERIALIZATION_ERROR";
constexpr auto kInternalReason = "INTERNAL_ERROR";
constexpr auto kErrorDomain = "a2a-protocol.org";
constexpr auto kErrorInfoType = "type.googleapis.com/google.rpc.ErrorInfo";
constexpr auto kErrorCodeMetadata = "a2a-error-code";
constexpr auto kStatusDetailsMetadata = "grpc-status-details-bin";
constexpr auto kInvalidListHistory = "ListTasksRequest.history_length must be non-negative";
::grpc::StatusCode RemoteProtocolStatusCode(const core::Error& error);

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
  if (error.http_status() == core::http::kStatusUnauthorized) {
    return ::grpc::StatusCode::UNAUTHENTICATED;
  }
  if (error.http_status() == core::http::kStatusForbidden) {
    return ::grpc::StatusCode::PERMISSION_DENIED;
  }
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
      return kValidationCode;
    case core::ErrorCode::kUnsupportedVersion:
      return kUnsupportedVersionCode;
    case core::ErrorCode::kNetwork:
      return kNetworkCode;
    case core::ErrorCode::kRemoteProtocol:
      return kRemoteProtocolCode;
    case core::ErrorCode::kSerialization:
      return kSerializationCode;
    case core::ErrorCode::kInternal:
      return kInternalCode;
  }
  return kInternalCode;
}

std::string ErrorInfoReason(const core::Error& error) {
  const auto& protocol_code = error.protocol_code();
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kTaskNotFound) {
    return kTaskNotFoundReason;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kTaskNotCancelable) {
    return kTaskNotCancelableReason;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kPushNotificationNotSupported) {
    return kPushNotSupportedReason;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kUnsupportedOperation) {
    return kUnsupportedOperationReason;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kExtendedAgentCardNotConfigured) {
    return kExtendedCardNotConfiguredReason;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kExtensionSupportRequired) {
    return kExtensionRequiredReason;
  }
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return kValidationReason;
    case core::ErrorCode::kUnsupportedVersion:
      return kVersionReason;
    case core::ErrorCode::kNetwork:
      return kNetworkReason;
    case core::ErrorCode::kRemoteProtocol:
      return kRemoteProtocolReason;
    case core::ErrorCode::kSerialization:
      return kSerializationReason;
    case core::ErrorCode::kInternal:
      return kInternalReason;
  }
  return kInternalReason;
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
constexpr std::string_view kExtensionsMetadataKey = "a2a-extensions";

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
  AppendLengthDelimited(message, 2U, kErrorDomain);
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
  AppendLengthDelimited(message, 3U, SerializeAny(kErrorInfoType, SerializeErrorInfo(error)));
  return message;
}

}  // namespace
void AddGrpcActivatedExtensions(::grpc::ServerContextBase* context,
                                const std::vector<std::string>& activated_extensions) {
  if (context == nullptr || activated_extensions.empty()) {
    return;
  }
  context->AddTrailingMetadata(std::string(kExtensionsMetadataKey), core::Extensions::Format(activated_extensions));
}

core::Result<ValidatedGrpcRequest> BuildGrpcRequestContext(const ::grpc::ServerContextBase& context,
                                                           const RequiredExtensionsValidator& extensions_validator) {
  RequestContext request_context;
  request_context.remote_address = context.peer();

  for (const auto& [key_ref, value_ref] : context.client_metadata()) {
    const std::string key(key_ref.data(), key_ref.length());
    const std::string value(value_ref.data(), value_ref.length());
    request_context.client_headers[key] = value;
  }
  request_context.auth_metadata = ExtractAuthMetadata(request_context.client_headers);
  const auto version = VersionHeaderValidator{}.Validate(request_context.client_headers);
  if (!version.ok()) {
    return version.error().WithTransport(std::string(GrpcServerTransport::kTransportName));
  }
  request_context.protocol_version = core::Version::kProtocolVersion;

  const auto extensions = extensions_validator.Validate(request_context.client_headers);
  if (!extensions.ok()) {
    return extensions.error().WithTransport(std::string(GrpcServerTransport::kTransportName));
  }
  return ValidatedGrpcRequest{.request_context = std::move(request_context),
                              .activated_extensions = extensions.value()};
}
::grpc::Status GrpcErrorStatus(const core::Error& error, ::grpc::ServerContextBase* context,
                               const std::vector<std::string>& activated_extensions,
                               std::optional<::grpc::StatusCode> status_override) {
  const auto status_code = status_override.value_or(ToStatusCode(error));
  if (context != nullptr) {
    AddGrpcActivatedExtensions(context, activated_extensions);
    context->AddTrailingMetadata(kErrorCodeMetadata, ErrorCodeName(error.code()));
    context->AddTrailingMetadata(kStatusDetailsMetadata, SerializeGrpcStatusDetails(status_code, error));
    const auto& protocol_code = error.protocol_code();
    if (protocol_code.has_value()) {
      context->AddTrailingMetadata(std::string(GrpcServerTransport::kProtocolCodeMetadataKey), *protocol_code);
    }
  }

  return {status_code, std::string(error.message())};
}
core::Result<ListTasksRequest> ParseGrpcListTasksRequest(const lf::a2a::v1::ListTasksRequest& request) {
  ListTasksRequest list_request;
  const auto page_size = NormalizeListTasksPageSize(
      request.has_page_size() ? std::optional<std::int64_t>{request.page_size()} : std::nullopt);
  if (!page_size.ok()) {
    return page_size.error();
  }
  list_request.page_size = page_size.value();
  list_request.page_token = request.page_token();
  list_request.context_id = request.context_id();
  if (request.status() != lf::a2a::v1::TASK_STATE_UNSPECIFIED) {
    list_request.status_filter = request.status();
  }
  if (request.has_history_length()) {
    if (request.history_length() < 0) {
      return core::Error::Validation(kInvalidListHistory);
    }
    list_request.history_length = static_cast<std::size_t>(request.history_length());
  }
  if (request.has_include_artifacts()) {
    list_request.include_artifacts = request.include_artifacts();
  }
  if (request.has_status_timestamp_after()) {
    list_request.status_timestamp_after = request.status_timestamp_after();
  }

  return list_request;
}
void WriteGrpcListTasksResponse(const ListTasksResponse& payload, lf::a2a::v1::ListTasksResponse& response) {
  for (const auto& task : payload.tasks) {
    *response.add_tasks() = task;
  }
  response.set_page_size(static_cast<int32_t>(payload.page_size));
  response.set_total_size(static_cast<int32_t>(payload.total_size));
  response.set_next_page_token(payload.next_page_token);
}
}  // namespace a2a::server
