// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string_view>

namespace a2a::core::protocol_methods {

inline constexpr std::string_view kSendMessage = "SendMessage";
inline constexpr std::string_view kSendStreamingMessage = "SendStreamingMessage";
inline constexpr std::string_view kGetTask = "GetTask";
inline constexpr std::string_view kListTasks = "ListTasks";
inline constexpr std::string_view kCancelTask = "CancelTask";
inline constexpr std::string_view kSubscribeToTask = "SubscribeToTask";
inline constexpr std::string_view kCreateTaskPushNotificationConfig = "CreateTaskPushNotificationConfig";
inline constexpr std::string_view kGetTaskPushNotificationConfig = "GetTaskPushNotificationConfig";
inline constexpr std::string_view kListTaskPushNotificationConfigs = "ListTaskPushNotificationConfigs";
inline constexpr std::string_view kDeleteTaskPushNotificationConfig = "DeleteTaskPushNotificationConfig";
inline constexpr std::string_view kPushNotificationConfigsSegment = "/pushNotificationConfigs";

struct GetExtendedAgentCardMethodName final {
  static constexpr std::string_view kCanonical = "GetExtendedAgentCard";
  static constexpr std::string_view kJsonRpcAlias = "a2a.getExtendedAgentCard";

  constexpr operator std::string_view() const noexcept { return kCanonical; }
};

inline constexpr GetExtendedAgentCardMethodName kGetExtendedAgentCard{};

constexpr bool operator==(std::string_view actual, GetExtendedAgentCardMethodName) noexcept {
  return actual == GetExtendedAgentCardMethodName::kCanonical ||
         actual == GetExtendedAgentCardMethodName::kJsonRpcAlias;
}

constexpr bool operator==(GetExtendedAgentCardMethodName method, std::string_view actual) noexcept {
  return actual == method;
}

}  // namespace a2a::core::protocol_methods
