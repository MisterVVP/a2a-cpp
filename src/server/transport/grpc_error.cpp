// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/transport/grpc_error.h"

#include <grpcpp/server_context.h>

#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/server/grpc_server_transport.h"
#include "server/transport/grpc_request_context.h"
#include "server/transport/grpc_status_details.h"

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
constexpr auto kErrorCodeMetadata = "a2a-error-code";
constexpr auto kStatusDetailsMetadata = "grpc-status-details-bin";
::grpc::StatusCode RemoteProtocolStatusCode(const core::Error& error);
}  // namespace

::grpc::StatusCode GrpcStatusCode(const core::Error& error) {
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

namespace {
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
}  // namespace

std::string GrpcErrorCodeName(core::ErrorCode code) {
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

std::string GrpcErrorInfoReason(const core::Error& error) {
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

::grpc::Status GrpcErrorStatus(const core::Error& error, ::grpc::ServerContextBase* context,
                               const std::vector<std::string>& activated_extensions,
                               std::optional<::grpc::StatusCode> status_override) {
  const auto status_code = status_override.value_or(GrpcStatusCode(error));
  if (context != nullptr) {
    AddGrpcActivatedExtensions(context, activated_extensions);
    context->AddTrailingMetadata(kErrorCodeMetadata, GrpcErrorCodeName(error.code()));
    context->AddTrailingMetadata(kStatusDetailsMetadata,
                                 SerializeGrpcStatusDetails(status_code, error.message(), GrpcErrorInfoReason(error)));
    const auto& protocol_code = error.protocol_code();
    if (protocol_code.has_value()) {
      context->AddTrailingMetadata(std::string(GrpcServerTransport::kProtocolCodeMetadataKey), *protocol_code);
    }
  }

  return {status_code, std::string(error.message())};
}
}  // namespace a2a::server
