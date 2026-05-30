// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_store.h"

#include <string>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"

namespace a2a::server {
namespace {

core::Result<void> ValidateConfig(const lf::a2a::v1::TaskPushNotificationConfig& config) {
  if (config.task_id().empty()) {
    return core::Error::Validation("push notification task_id is required");
  }
  if (config.id().empty()) {
    return core::Error::Validation("push notification id is required");
  }
  if (config.url().empty()) {
    return core::Error::Validation("push notification url is required");
  }
  return {};
}

core::Result<void> ValidateLookup(std::string_view task_id, std::string_view config_id) {
  if (task_id.empty()) {
    return core::Error::Validation("push notification task_id is required");
  }
  if (config_id.empty()) {
    return core::Error::Validation("push notification id is required");
  }
  return {};
}

}  // namespace

core::Result<lf::a2a::v1::TaskPushNotificationConfig> InMemoryPushNotificationStore::CreateOrUpdate(
    const lf::a2a::v1::TaskPushNotificationConfig& config) {
  const auto validation = ValidateConfig(config);
  if (!validation.ok()) {
    return validation.error();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  configs_[config.task_id()][config.id()] = config;
  return config;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> InMemoryPushNotificationStore::Get(
    std::string_view task_id, std::string_view config_id) const {
  const auto validation = ValidateLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto task_it = configs_.find(std::string(task_id));
  if (task_it == configs_.end()) {
    return core::protocol_errors::TaskNotFound("push notification task config not found");
  }
  const auto config_it = task_it->second.find(std::string(config_id));
  if (config_it == task_it->second.end()) {
    return core::Error::Validation("push notification config not found");
  }
  return config_it->second;
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> InMemoryPushNotificationStore::List(
    std::string_view task_id) const {
  if (task_id.empty()) {
    return core::Error::Validation("push notification task_id is required");
  }

  lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto task_it = configs_.find(std::string(task_id));
  if (task_it == configs_.end()) {
    return response;
  }
  for (const auto& [unused_id, config] : task_it->second) {
    (void)unused_id;
    *response.add_configs() = config;
  }
  return response;
}

core::Result<void> InMemoryPushNotificationStore::Delete(std::string_view task_id, std::string_view config_id) {
  const auto validation = ValidateLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto task_it = configs_.find(std::string(task_id));
  if (task_it == configs_.end()) {
    return {};
  }
  task_it->second.erase(std::string(config_id));
  if (task_it->second.empty()) {
    configs_.erase(task_it);
  }
  return {};
}

}  // namespace a2a::server
