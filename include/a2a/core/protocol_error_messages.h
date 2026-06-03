// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace a2a::core::protocol_error_messages {

inline constexpr char kEmptyPrefix[] = "";
inline constexpr char kUnexpectedDispatchPayloadTypePrefix[] = "Unexpected dispatch payload type for ";
inline constexpr char kDispatchPayloadTypeMismatchPrefix[] = "Dispatch payload type mismatch for ";
inline constexpr char kResponsePayloadMismatchSuffix[] = " response payload mismatch";
inline constexpr char kJsonRpcResponsePayloadMismatchPrefix[] = "JSON-RPC ";

template <std::size_t PrefixSize, std::size_t ValueSize, std::size_t SuffixSize>
[[nodiscard]] consteval auto MakeMessage(const char (&prefix)[PrefixSize], const char (&value)[ValueSize],
                                         const char (&suffix)[SuffixSize]) {
  std::array<char, PrefixSize + ValueSize + SuffixSize - 2U> message{};
  std::size_t index = 0;
  for (std::size_t prefix_index = 0; prefix_index + 1U < PrefixSize; ++prefix_index) {
    message[index] = prefix[prefix_index];
    ++index;
  }
  for (std::size_t value_index = 0; value_index + 1U < ValueSize; ++value_index) {
    message[index] = value[value_index];
    ++index;
  }
  for (std::size_t suffix_index = 0; suffix_index < SuffixSize; ++suffix_index) {
    message[index + suffix_index] = suffix[suffix_index];
  }
  return message;
}

template <std::size_t OperationSize>
[[nodiscard]] consteval auto MakeUnexpectedDispatchPayloadType(const char (&operation)[OperationSize]) {
  return MakeMessage(kUnexpectedDispatchPayloadTypePrefix, operation, kEmptyPrefix);
}

template <std::size_t OperationSize>
[[nodiscard]] consteval auto MakeDispatchPayloadTypeMismatch(const char (&operation)[OperationSize]) {
  return MakeMessage(kDispatchPayloadTypeMismatchPrefix, operation, kEmptyPrefix);
}

template <std::size_t PayloadSize>
[[nodiscard]] consteval auto MakeResponsePayloadMismatch(const char (&payload)[PayloadSize]) {
  return MakeMessage(kEmptyPrefix, payload, kResponsePayloadMismatchSuffix);
}

template <std::size_t PayloadSize>
[[nodiscard]] consteval auto MakeJsonRpcResponsePayloadMismatch(const char (&payload)[PayloadSize]) {
  return MakeMessage(kJsonRpcResponsePayloadMismatchPrefix, payload, kResponsePayloadMismatchSuffix);
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

inline constexpr auto kDispatchPayloadTypeMismatchForSendMessage = MakeDispatchPayloadTypeMismatch("SendMessage");
inline constexpr auto kDispatchPayloadTypeMismatchForSendStreamingMessage =
    MakeDispatchPayloadTypeMismatch("SendStreamingMessage");
inline constexpr auto kDispatchPayloadTypeMismatchForGetTask = MakeDispatchPayloadTypeMismatch("GetTask");
inline constexpr auto kDispatchPayloadTypeMismatchForCancelTask = MakeDispatchPayloadTypeMismatch("CancelTask");
inline constexpr auto kDispatchPayloadTypeMismatchForListTasks = MakeDispatchPayloadTypeMismatch("ListTasks");
inline constexpr auto kDispatchPayloadTypeMismatchForCreateTaskPushNotificationConfig =
    MakeDispatchPayloadTypeMismatch("CreateTaskPushNotificationConfig");
inline constexpr auto kDispatchPayloadTypeMismatchForGetTaskPushNotificationConfig =
    MakeDispatchPayloadTypeMismatch("GetTaskPushNotificationConfig");
inline constexpr auto kDispatchPayloadTypeMismatchForListTaskPushNotificationConfigs =
    MakeDispatchPayloadTypeMismatch("ListTaskPushNotificationConfigs");
inline constexpr auto kDispatchPayloadTypeMismatchForDeleteTaskPushNotificationConfig =
    MakeDispatchPayloadTypeMismatch("DeleteTaskPushNotificationConfig");

inline constexpr auto kResponsePayloadMismatchForSendMessage = MakeResponsePayloadMismatch("SendMessage");
inline constexpr auto kResponsePayloadMismatchForSendStreamingMessage =
    MakeResponsePayloadMismatch("SendStreamingMessage");
inline constexpr auto kResponsePayloadMismatchForTask = MakeResponsePayloadMismatch("Task");
inline constexpr auto kResponsePayloadMismatchForListTasks = MakeResponsePayloadMismatch("ListTasks");
inline constexpr auto kResponsePayloadMismatchForPushConfig = MakeResponsePayloadMismatch("Push config");
inline constexpr auto kResponsePayloadMismatchForPushConfigList = MakeResponsePayloadMismatch("Push config list");

inline constexpr auto kJsonRpcResponsePayloadMismatchForSendMessage = MakeJsonRpcResponsePayloadMismatch("SendMessage");
inline constexpr auto kJsonRpcResponsePayloadMismatchForStreaming = MakeJsonRpcResponsePayloadMismatch("streaming");
inline constexpr auto kJsonRpcResponsePayloadMismatchForSubscribe = MakeJsonRpcResponsePayloadMismatch("subscribe");
inline constexpr auto kJsonRpcResponsePayloadMismatchForTask = MakeJsonRpcResponsePayloadMismatch("Task");
inline constexpr auto kJsonRpcResponsePayloadMismatchForListTasks = MakeJsonRpcResponsePayloadMismatch("ListTasks");
inline constexpr auto kJsonRpcResponsePayloadMismatchForPushConfig = MakeJsonRpcResponsePayloadMismatch("push config");
inline constexpr auto kJsonRpcResponsePayloadMismatchForPushConfigList =
    MakeJsonRpcResponsePayloadMismatch("push config list");

}  // namespace a2a::core::protocol_error_messages
