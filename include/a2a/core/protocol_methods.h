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
inline constexpr std::string_view kGetExtendedAgentCard = "GetExtendedAgentCard";
inline constexpr std::string_view kPushNotificationConfigsSegment = "/pushNotificationConfigs";

}  // namespace a2a::core::protocol_methods
