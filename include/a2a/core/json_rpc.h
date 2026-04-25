#pragma once

#include <string_view>

namespace a2a::core::json_rpc {

constexpr std::string_view kVersion = "2.0";

struct MethodNames final {
  static constexpr std::string_view kSendMessage = "a2a.sendMessage";
  static constexpr std::string_view kGetTask = "a2a.getTask";
  static constexpr std::string_view kCancelTask = "a2a.cancelTask";
  static constexpr std::string_view kSetTaskPushNotificationConfig =
      "a2a.setTaskPushNotificationConfig";
  static constexpr std::string_view kGetTaskPushNotificationConfig =
      "a2a.getTaskPushNotificationConfig";
  static constexpr std::string_view kListTaskPushNotificationConfigs =
      "a2a.listTaskPushNotificationConfigs";
  static constexpr std::string_view kDeleteTaskPushNotificationConfig =
      "a2a.deleteTaskPushNotificationConfig";
  static constexpr std::string_view kListTasks = "a2a.listTasks";
};

}  // namespace a2a::core::json_rpc
