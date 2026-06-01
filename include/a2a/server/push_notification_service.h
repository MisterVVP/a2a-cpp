// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string_view>

#include "a2a/core/result.h"
#include "a2a/server/push_notification_delivery.h"
#include "a2a/server/push_notification_store.h"
#include "a2a/server/server.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class PushNotificationService final {
 public:
  static constexpr std::string_view kPushDeliveryNonSuccessStatusMessage =
      "push notification delivery returned non-2xx status";

  PushNotificationService(TaskStore* task_store, PushNotificationStore* push_store,
                          PushNotificationDeliveryClient* delivery_client);

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& config) const;
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request) const;
  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request) const;
  [[nodiscard]] core::Result<void> DeleteConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request) const;
  [[nodiscard]] core::Result<void> RegisterInlineConfigIfPresent(const lf::a2a::v1::SendMessageRequest& request,
                                                                 std::string_view resolved_task_id) const;
  [[nodiscard]] core::Result<void> NotifyTaskUpdated(const lf::a2a::v1::Task& task) const;

 private:
  TaskStore* task_store_ = nullptr;
  PushNotificationStore* push_store_ = nullptr;
  PushNotificationDeliveryClient* delivery_client_ = nullptr;
};

}  // namespace a2a::server
