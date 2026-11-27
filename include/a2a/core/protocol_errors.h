// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_codes.h"

namespace a2a::core::protocol_errors {

inline constexpr char kDefaultUnauthenticatedMessage[] = "authentication is required";
inline constexpr char kDefaultPermissionDeniedMessage[] = "access is denied";
inline constexpr char kDefaultTaskNotFoundMessage[] = "task not found";
inline constexpr char kDefaultTaskNotCancelableMessage[] = "task is already terminal";
inline constexpr char kDefaultPushNotSupportedMessage[] = "push notifications are not supported";
inline constexpr char kDefaultUnsupportedOperationMessage[] = "operation is not supported";
inline constexpr char kDefaultContentTypeNotSupportedMessage[] = "content type is not supported";
inline constexpr char kDefaultInvalidAgentResponseMessage[] = "invalid agent response";
inline constexpr char kDefaultExtendedCardNotConfiguredMessage[] = "extended agent card is not configured";
inline constexpr char kDefaultVersionNotSupportedMessage[] = "version is not supported";
inline constexpr char kDefaultExtensionRequiredMessage[] = "required extension support is missing";

[[nodiscard]] inline Error Unauthenticated(std::string message = kDefaultUnauthenticatedMessage) {
  return Error::RemoteProtocol(std::move(message)).WithHttpStatus(http::kStatusUnauthorized);
}

[[nodiscard]] inline Error PermissionDenied(std::string message = kDefaultPermissionDeniedMessage) {
  return Error::RemoteProtocol(std::move(message)).WithHttpStatus(http::kStatusForbidden);
}

[[nodiscard]] inline Error TaskNotFound(std::string message = kDefaultTaskNotFoundMessage) {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(http::kStatusNotFound)
      .WithProtocolCode(std::string(protocol_codes::kTaskNotFound));
}

[[nodiscard]] inline Error TaskNotCancelable(std::string message = kDefaultTaskNotCancelableMessage) {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(http::kStatusBadRequest)
      .WithProtocolCode(std::string(protocol_codes::kTaskNotCancelable));
}

[[nodiscard]] inline Error PushNotificationNotSupported(std::string message = kDefaultPushNotSupportedMessage) {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(http::kStatusBadRequest)
      .WithProtocolCode(std::string(protocol_codes::kPushNotificationNotSupported));
}

[[nodiscard]] inline Error UnsupportedOperation(std::string message = kDefaultUnsupportedOperationMessage) {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(http::kStatusBadRequest)
      .WithProtocolCode(std::string(protocol_codes::kUnsupportedOperation));
}

[[nodiscard]] inline Error ContentTypeNotSupported(std::string message = kDefaultContentTypeNotSupportedMessage) {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(http::kStatusBadRequest)
      .WithProtocolCode(std::string(protocol_codes::kContentTypeNotSupported));
}

[[nodiscard]] inline Error InvalidAgentResponse(std::string message = kDefaultInvalidAgentResponseMessage) {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(http::kStatusInternalServerError)
      .WithProtocolCode(std::string(protocol_codes::kInvalidAgentResponse));
}

[[nodiscard]] inline Error ExtendedAgentCardNotConfigured(
    std::string message = kDefaultExtendedCardNotConfiguredMessage) {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(http::kStatusBadRequest)
      .WithProtocolCode(std::string(protocol_codes::kExtendedAgentCardNotConfigured));
}

[[nodiscard]] inline Error VersionNotSupported(std::string message = kDefaultVersionNotSupportedMessage) {
  return Error::UnsupportedVersion(std::move(message))
      .WithHttpStatus(http::kStatusBadRequest)
      .WithProtocolCode(std::string(protocol_codes::kVersionNotSupported));
}

[[nodiscard]] inline Error ExtensionSupportRequired(std::string message = kDefaultExtensionRequiredMessage) {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(http::kStatusBadRequest)
      .WithProtocolCode(std::string(protocol_codes::kExtensionSupportRequired));
}

}  // namespace a2a::core::protocol_errors
