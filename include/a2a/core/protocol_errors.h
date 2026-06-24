// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_codes.h"

namespace a2a::core::protocol_errors {

[[nodiscard]] inline Error TaskNotFound(std::string message = "task not found") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(404)
      .WithProtocolCode(std::string(protocol_codes::kTaskNotFound));
}

[[nodiscard]] inline Error TaskNotCancelable(std::string message = "task is already terminal") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(409)
      .WithProtocolCode(std::string(protocol_codes::kTaskNotCancelable));
}

[[nodiscard]] inline Error PushNotificationNotSupported(std::string message = "push notifications are not supported") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(400)
      .WithProtocolCode(std::string(protocol_codes::kPushNotificationNotSupported));
}

[[nodiscard]] inline Error UnsupportedOperation(std::string message = "operation is not supported") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(400)
      .WithProtocolCode(std::string(protocol_codes::kUnsupportedOperation));
}

[[nodiscard]] inline Error ContentTypeNotSupported(std::string message = "content type is not supported") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(415)
      .WithProtocolCode(std::string(protocol_codes::kContentTypeNotSupported));
}

[[nodiscard]] inline Error InvalidAgentResponse(std::string message = "invalid agent response") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(502)
      .WithProtocolCode(std::string(protocol_codes::kInvalidAgentResponse));
}

[[nodiscard]] inline Error ExtendedAgentCardNotConfigured(std::string message = "extended agent card is not configured") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(400)
      .WithProtocolCode(std::string(protocol_codes::kExtendedAgentCardNotConfigured));
}

[[nodiscard]] inline Error VersionNotSupported(std::string message = "version is not supported") {
  return Error::UnsupportedVersion(std::move(message))
      .WithHttpStatus(400)
      .WithProtocolCode(std::string(protocol_codes::kVersionNotSupported));
}

[[nodiscard]] inline Error ExtensionSupportRequired(std::string message = "required extension support is missing") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(400)
      .WithProtocolCode(std::string(protocol_codes::kExtensionSupportRequired));
}

}  // namespace a2a::core::protocol_errors
