// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/server/push_notification_service.h"
#include "a2a/server/push_notification_store.h"
#include "a2a/server/tasks/in_memory_task_store.h"

namespace {

constexpr std::string_view kTaskId = "push-demo-task";
constexpr std::string_view kContextId = "push-demo-context";
constexpr std::string_view kConfigId = "push-demo-config";
constexpr std::string_view kWebhookUrl = "https://webhook.example.test/a2a/task-updates";
constexpr std::string_view kAuthScheme = "Bearer";
constexpr std::string_view kCredentials = "example-token";
constexpr std::string_view kDeliveryCountLabel = "delivered push notifications: ";
constexpr std::string_view kWebhookLabel = "webhook url: ";
constexpr std::string_view kTaskLabel = "payload task id: ";
constexpr std::string_view kStateLabel = "payload task state: ";
constexpr std::string_view kErrorPrefix = "push notification example failed: ";
constexpr int kDeliveryHttpStatus = a2a::core::http::kStatusAccepted;
constexpr int kExpectedDeliveryCount = 1;

class RecordingPushDeliveryClient final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    requests.push_back(request);
    return a2a::server::PushDeliveryResult{.http_status = kDeliveryHttpStatus, .error_message = {}};
  }

  std::vector<a2a::server::PushDeliveryRequest> requests;
};

lf::a2a::v1::Task BuildCompletedTask() {
  lf::a2a::v1::Task task;
  task.set_id(std::string(kTaskId));
  task.set_context_id(std::string(kContextId));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  return task;
}

lf::a2a::v1::TaskPushNotificationConfig BuildPushConfig() {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_id(std::string(kConfigId));
  config.set_task_id(std::string(kTaskId));
  config.set_url(std::string(kWebhookUrl));
  config.mutable_authentication()->set_scheme(std::string(kAuthScheme));
  config.mutable_authentication()->set_credentials(std::string(kCredentials));
  return config;
}

}  // namespace

int main() {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingPushDeliveryClient delivery;
  a2a::server::PushNotificationService push_notifications(&task_store, &push_store, &delivery);

  const auto task = BuildCompletedTask();
  const auto stored_task = task_store.CreateOrUpdate(task);
  if (!stored_task.ok()) {
    std::cerr << kErrorPrefix << stored_task.error().message() << '\n';
    return 1;
  }

  const auto stored_config = push_notifications.CreateConfig(BuildPushConfig());
  if (!stored_config.ok()) {
    std::cerr << kErrorPrefix << stored_config.error().message() << '\n';
    return 1;
  }

  const auto notified = push_notifications.NotifyTaskUpdated(task);
  if (!notified.ok()) {
    std::cerr << kErrorPrefix << notified.error().message() << '\n';
    return 1;
  }
  if (delivery.requests.size() != static_cast<std::size_t>(kExpectedDeliveryCount)) {
    std::cerr << kErrorPrefix << kDeliveryCountLabel << delivery.requests.size() << '\n';
    return 1;
  }

  const auto& request = delivery.requests.front();
  std::cout << kDeliveryCountLabel << delivery.requests.size() << '\n';
  std::cout << kWebhookLabel << request.config.url() << '\n';
  std::cout << kTaskLabel << request.payload.status_update().task_id() << '\n';
  std::cout << kStateLabel << request.payload.status_update().status().state() << '\n';
  return 0;
}
