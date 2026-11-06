// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/transport/grpc_error.h"

#include <grpcpp/server_context.h>

#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protocol_error_reasons.h"
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
constexpr auto kErrorCodeMetadata = "a2a-error-code";
constexpr auto kStatusDetailsMetadata = "grpc-status-details-bin";
::grpc::StatusCode RemoteProtocolStatusCode(const core::Error& error);
}  // namespace

::grpc::StatusCode GrpcStatusCode(const core::Error& error) {
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return ::grpc::StatusCode::INVALID_ARGUMENT;
    case core::ErrorCode::kUnsupportedVersion:
      return ::grpc::StatusCode::FAILED_PRECONDITION;
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
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kContentTypeNotSupported) {
    return ::grpc::StatusCode::INVALID_ARGUMENT;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kInvalidAgentResponse) {
    return ::grpc::StatusCode::INTERNAL;
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
    return core::protocol_error_reasons::kTaskNotFound;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kTaskNotCancelable) {
    return core::protocol_error_reasons::kTaskNotCancelable;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kPushNotificationNotSupported) {
    return core::protocol_error_reasons::kPushNotificationNotSupported;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kUnsupportedOperation) {
    return core::protocol_error_reasons::kUnsupportedOperation;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kExtendedAgentCardNotConfigured) {
    return core::protocol_error_reasons::kExtendedAgentCardNotConfigured;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kExtensionSupportRequired) {
    return core::protocol_error_reasons::kExtensionSupportRequired;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kContentTypeNotSupported) {
    return core::protocol_error_reasons::kContentTypeNotSupported;
  }
  if (protocol_code.has_value() && *protocol_code == core::protocol_codes::kInvalidAgentResponse) {
    return core::protocol_error_reasons::kInvalidAgentResponse;
  }
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return core::protocol_error_reasons::kValidation;
    case core::ErrorCode::kUnsupportedVersion:
      return core::protocol_error_reasons::kVersionNotSupported;
    case core::ErrorCode::kNetwork:
      return core::protocol_error_reasons::kNetwork;
    case core::ErrorCode::kRemoteProtocol:
      return core::protocol_error_reasons::kRemoteProtocol;
    case core::ErrorCode::kSerialization:
      return core::protocol_error_reasons::kSerialization;
    case core::ErrorCode::kInternal:
      return core::protocol_error_reasons::kInternal;
  }
  return core::protocol_error_reasons::kInternal;
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
