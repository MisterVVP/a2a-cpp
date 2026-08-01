// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_service.h"

#include <string>
#include <string_view>

#include "a2a/core/error.h"
#include "a2a/core/http_constants.h"

namespace a2a::server {

lf::a2a::v1::StreamResponse BuildTaskStatusUpdatePayload(const lf::a2a::v1::Task& task) {
  lf::a2a::v1::StreamResponse payload;
  auto* update = payload.mutable_status_update();
  update->set_task_id(task.id());
  update->set_context_id(task.context_id());
  *update->mutable_status() = task.status();
  return payload;
}

PushNotificationService::PushNotificationService(TaskStore* task_store, PushNotificationStore* push_store,
                                                 PushNotificationDeliveryClient* delivery_client)
    : task_store_(task_store), push_store_(push_store), delivery_client_(delivery_client) {}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PushNotificationService::CreateConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& config) const {
  if (task_store_ == nullptr || push_store_ == nullptr) {
    return core::Error::Internal("push notification service is not configured");
  }
  const auto task = task_store_->Get(config.task_id());
  if (!task.ok()) {
    return task.error();
  }
  return push_store_->CreateOrUpdate(config);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PushNotificationService::GetConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request) const {
  if (push_store_ == nullptr) {
    return core::Error::Internal("push notification store is not configured");
  }
  return push_store_->Get(request.task_id(), request.id());
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> PushNotificationService::ListConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request) const {
  if (task_store_ == nullptr || push_store_ == nullptr) {
    return core::Error::Internal("push notification service is not configured");
  }
  if (!push_store_->ListValidatesTaskExistence()) {
    const auto task = task_store_->Get(request.task_id());
    if (!task.ok()) {
      return task.error();
    }
  }
  return push_store_->List(request.task_id(), request.page_size(), request.page_token());
}

core::Result<void> PushNotificationService::DeleteConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request) const {
  if (push_store_ == nullptr) {
    return core::Error::Internal("push notification store is not configured");
  }
  return push_store_->Delete(request.task_id(), request.id());
}

core::Result<void> PushNotificationService::RegisterInlineConfigIfPresent(
    const lf::a2a::v1::SendMessageRequest& request, std::string_view resolved_task_id) const {
  if (!request.has_configuration() || !request.configuration().has_task_push_notification_config()) {
    return {};
  }
  lf::a2a::v1::TaskPushNotificationConfig config = request.configuration().task_push_notification_config();
  config.set_task_id(std::string(resolved_task_id));
  const auto created = CreateConfig(config);
  if (!created.ok()) {
    return created.error();
  }
  return {};
}

core::Result<void> PushNotificationService::NotifyTaskUpdated(const lf::a2a::v1::Task& task) const {
  if (push_store_ == nullptr || delivery_client_ == nullptr) {
    return core::Error::Internal("push notification delivery is not configured");
  }
  auto configs = push_store_->List(task.id());
  if (!configs.ok()) {
    return configs.error();
  }

  const lf::a2a::v1::StreamResponse payload = BuildTaskStatusUpdatePayload(task);

  for (const auto& config : configs.value().configs()) {
    PushDeliveryRequest request{.config = config, .payload = payload};
    const auto delivered = delivery_client_->Deliver(request);
    if (!delivered.ok()) {
      return delivered.error();
    }
    const auto& delivery_result = delivered.value();
    if (!delivery_result.error_message.empty()) {
      return core::Error::RemoteProtocol(delivery_result.error_message).WithHttpStatus(delivery_result.http_status);
    }
    if (delivery_result.http_status < core::http::kSuccessStatusMin ||
        delivery_result.http_status > core::http::kSuccessStatusMax) {
      return core::Error::RemoteProtocol(std::string{kPushDeliveryNonSuccessStatusMessage})
          .WithHttpStatus(delivery_result.http_status);
    }
  }
  return {};
}

}  // namespace a2a::server
