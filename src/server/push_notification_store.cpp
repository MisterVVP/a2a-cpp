// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_store.h"

#include <memory>
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

void InMemoryPushNotificationStore::RefreshSnapshot(TaskConfigs* task_configs) {
  task_configs->list_snapshot = std::make_shared<ListResponse>(task_configs->list_response);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> InMemoryPushNotificationStore::CreateOrUpdate(
    const lf::a2a::v1::TaskPushNotificationConfig& config) {
  const auto validation = ValidateConfig(config);
  if (!validation.ok()) {
    return validation.error();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto [task_it, unused_inserted] = configs_.try_emplace(config.task_id());
  (void)unused_inserted;
  auto& task_configs = task_it->second;
  const auto config_it = task_configs.config_indices.find(config.id());
  if (config_it != task_configs.config_indices.end()) {
    *task_configs.list_response.mutable_configs(config_it->second) = config;
    RefreshSnapshot(&task_configs);
    return config;
  }

  const int config_index = task_configs.list_response.configs_size();
  *task_configs.list_response.add_configs() = config;
  task_configs.config_indices.emplace(config.id(), config_index);
  RefreshSnapshot(&task_configs);
  return config;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> InMemoryPushNotificationStore::Get(
    std::string_view task_id, std::string_view config_id) const {
  const auto validation = ValidateLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto task_it = configs_.find(task_id);
  if (task_it == configs_.end()) {
    return core::protocol_errors::TaskNotFound("push notification task config not found");
  }
  const auto config_it = task_it->second.config_indices.find(config_id);
  if (config_it == task_it->second.config_indices.end()) {
    return core::Error::Validation("push notification config not found");
  }
  return task_it->second.list_response.configs(config_it->second);
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> InMemoryPushNotificationStore::List(
    std::string_view task_id) const {
  if (task_id.empty()) {
    return core::Error::Validation("push notification task_id is required");
  }

  std::shared_ptr<const ListResponse> list_snapshot;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto task_it = configs_.find(task_id);
    if (task_it == configs_.end()) {
      return lf::a2a::v1::ListTaskPushNotificationConfigsResponse{};
    }
    list_snapshot = task_it->second.list_snapshot;
  }
  return *list_snapshot;
}

core::Result<void> InMemoryPushNotificationStore::Delete(std::string_view task_id, std::string_view config_id) {
  const auto validation = ValidateLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto task_it = configs_.find(task_id);
  if (task_it == configs_.end()) {
    return {};
  }
  auto& task_configs = task_it->second;
  const auto config_it = task_configs.config_indices.find(config_id);
  if (config_it == task_configs.config_indices.end()) {
    return {};
  }

  const int config_index = config_it->second;
  const int last_index = task_configs.list_response.configs_size() - 1;
  if (config_index != last_index) {
    const lf::a2a::v1::TaskPushNotificationConfig last_config = task_configs.list_response.configs(last_index);
    *task_configs.list_response.mutable_configs(config_index) = last_config;
    task_configs.config_indices[last_config.id()] = config_index;
  }
  task_configs.list_response.mutable_configs()->RemoveLast();
  task_configs.config_indices.erase(config_it);

  if (task_configs.config_indices.empty()) {
    configs_.erase(task_it);
    return {};
  }

  RefreshSnapshot(&task_configs);
  return {};
}

}  // namespace a2a::server
