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

[[nodiscard]] inline Error PushNotificationNotSupported(
    std::string message = "push notifications are not supported") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(400)
      .WithProtocolCode(std::string(protocol_codes::kPushNotificationNotSupported));
}

[[nodiscard]] inline Error UnsupportedOperation(std::string message = "operation is not supported") {
  return Error::RemoteProtocol(std::move(message))
      .WithHttpStatus(400)
      .WithProtocolCode(std::string(protocol_codes::kUnsupportedOperation));
}

}  // namespace a2a::core::protocol_errors
