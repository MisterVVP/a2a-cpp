// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace a2a::core::protocol_error_messages {

inline constexpr std::string_view kUnexpectedDispatchPayloadTypePrefix = "Unexpected dispatch payload type for ";

template <std::size_t OperationSize>
[[nodiscard]] consteval auto MakeUnexpectedDispatchPayloadType(const char (&operation)[OperationSize]) {
  std::array<char, kUnexpectedDispatchPayloadTypePrefix.size() + OperationSize> message{};
  std::size_t index = 0;
  for (const char character : kUnexpectedDispatchPayloadTypePrefix) {
    message[index] = character;
    ++index;
  }
  for (std::size_t operation_index = 0; operation_index < OperationSize; ++operation_index) {
    message[index + operation_index] = operation[operation_index];
  }
  return message;
}

template <std::size_t MessageSize>
[[nodiscard]] std::string ToString(const std::array<char, MessageSize>& message) {
  static_assert(MessageSize > 0);
  return std::string(message.data(), MessageSize - 1);
}

inline constexpr auto kUnexpectedDispatchPayloadTypeForSendMessage = MakeUnexpectedDispatchPayloadType("SendMessage");
inline constexpr auto kUnexpectedDispatchPayloadTypeForSendStreamingMessage =
    MakeUnexpectedDispatchPayloadType("SendStreamingMessage");
inline constexpr auto kUnexpectedDispatchPayloadTypeForGetTask = MakeUnexpectedDispatchPayloadType("GetTask");
inline constexpr auto kUnexpectedDispatchPayloadTypeForCancelTask = MakeUnexpectedDispatchPayloadType("CancelTask");
inline constexpr auto kUnexpectedDispatchPayloadTypeForListTasks = MakeUnexpectedDispatchPayloadType("ListTasks");
inline constexpr auto kUnexpectedDispatchPayloadTypeForSubscribeToTask =
    MakeUnexpectedDispatchPayloadType("SubscribeToTask");
inline constexpr auto kUnexpectedDispatchPayloadTypeForCreateTaskPushNotificationConfig =
    MakeUnexpectedDispatchPayloadType("CreateTaskPushNotificationConfig");
inline constexpr auto kUnexpectedDispatchPayloadTypeForGetTaskPushNotificationConfig =
    MakeUnexpectedDispatchPayloadType("GetTaskPushNotificationConfig");
inline constexpr auto kUnexpectedDispatchPayloadTypeForListTaskPushNotificationConfigs =
    MakeUnexpectedDispatchPayloadType("ListTaskPushNotificationConfigs");

}  // namespace a2a::core::protocol_error_messages
