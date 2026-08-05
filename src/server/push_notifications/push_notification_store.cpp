// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_store.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/server/tasks/task_store.h"

namespace a2a::server {
namespace {

constexpr std::string_view kTaskIdRequiredMessage = "push notification task_id is required";
constexpr std::string_view kConfigIdRequiredMessage = "push notification id is required";
constexpr std::string_view kConfigUrlRequiredMessage = "push notification url is required";
constexpr std::string_view kTaskConfigNotFoundMessage = "push notification task config not found";
constexpr std::string_view kConfigNotFoundMessage = "push notification config not found";
constexpr std::string_view kPageSizeInvalidMessage =
    "ListTaskPushNotificationConfigsRequest.page_size must be non-negative";
constexpr std::string_view kPageTokenInvalidMessage =
    "ListTaskPushNotificationConfigsRequest.page_token must be a non-negative integer";
constexpr std::string_view kPageTokenOutOfRangeMessage =
    "ListTaskPushNotificationConfigsRequest.page_token exceeds available config count";

core::Result<void> ValidateConfig(const lf::a2a::v1::TaskPushNotificationConfig& config) {
  if (config.task_id().empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  if (config.id().empty()) {
    return core::Error::Validation(std::string(kConfigIdRequiredMessage));
  }
  if (config.url().empty()) {
    return core::Error::Validation(std::string(kConfigUrlRequiredMessage));
  }
  return {};
}

core::Result<void> ValidateLookup(std::string_view task_id, std::string_view config_id) {
  if (task_id.empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  if (config_id.empty()) {
    return core::Error::Validation(std::string(kConfigIdRequiredMessage));
  }
  return {};
}

core::Result<std::size_t> ParsePageToken(std::string_view page_token) {
  if (page_token.empty()) {
    return std::size_t{0};
  }
  std::size_t parsed = 0;
  const auto* begin = page_token.data();
  const auto* end = begin + page_token.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return core::Error::Validation(std::string(kPageTokenInvalidMessage));
  }
  return parsed;
}

core::Result<void> ValidateListRequest(std::string_view task_id, int page_size) {
  if (task_id.empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  if (page_size < 0) {
    return core::Error::Validation(std::string(kPageSizeInvalidMessage));
  }
  return {};
}

}  // namespace

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PushNotificationStore::CreateOrUpdateForTask(
    const lf::a2a::v1::TaskPushNotificationConfig& config, const TaskStore& task_store) {
  const auto task = task_store.Get(config.task_id());
  if (!task.ok()) {
    return task.error();
  }
  return CreateOrUpdate(config);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PushNotificationStore::GetForTask(
    std::string_view task_id, std::string_view config_id, const TaskStore& task_store) const {
  const auto validation = ValidateLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }
  const auto task = task_store.Get(task_id);
  if (!task.ok()) {
    return task.error();
  }
  return Get(task_id, config_id);
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> PushNotificationStore::ListForTask(
    std::string_view task_id, int page_size, std::string_view page_token, const TaskStore& task_store) const {
  const auto task = task_store.Get(task_id);
  if (!task.ok()) {
    return task.error();
  }
  return ListForExistingTask(task_id, page_size, page_token);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> InMemoryPushNotificationStore::CreateOrUpdate(
    const lf::a2a::v1::TaskPushNotificationConfig& config) {
  const auto validation = ValidateConfig(config);
  if (!validation.ok()) {
    return validation.error();
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  auto [task_it, unused_inserted] = configs_.try_emplace(config.task_id());
  (void)unused_inserted;
  auto& task_configs = task_it->second;
  const auto config_it = task_configs.config_indices.find(config.id());
  if (config_it != task_configs.config_indices.end()) {
    *task_configs.list_response.mutable_configs(config_it->second) = config;
    return config;
  }

  const int config_index = task_configs.list_response.configs_size();
  *task_configs.list_response.add_configs() = config;
  task_configs.config_indices.emplace(config.id(), config_index);
  return config;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> InMemoryPushNotificationStore::Get(
    std::string_view task_id, std::string_view config_id) const {
  const auto validation = ValidateLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto task_it = configs_.find(task_id);
  if (task_it == configs_.end()) {
    return core::protocol_errors::TaskNotFound(std::string(kTaskConfigNotFoundMessage));
  }
  const auto config_it = task_it->second.config_indices.find(config_id);
  if (config_it == task_it->second.config_indices.end()) {
    return core::Error::Validation(std::string(kConfigNotFoundMessage));
  }
  return task_it->second.list_response.configs(config_it->second);
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> InMemoryPushNotificationStore::List(
    std::string_view task_id, int page_size, std::string_view page_token) const {
  const auto validation = ValidateListRequest(task_id, page_size);
  if (!validation.ok()) {
    return validation.error();
  }
  const auto offset = ParsePageToken(page_token);
  if (!offset.ok()) {
    return offset.error();
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto task_it = configs_.find(task_id);
  if (task_it == configs_.end()) {
    return lf::a2a::v1::ListTaskPushNotificationConfigsResponse{};
  }

  const auto& source_configs = task_it->second.list_response.configs();
  const int source_config_count = source_configs.size();
  const std::size_t start = offset.value();
  if (std::cmp_greater(start, source_config_count)) {
    return core::Error::Validation(std::string(kPageTokenOutOfRangeMessage));
  }
  const std::size_t remaining = static_cast<std::size_t>(source_config_count) - start;
  const std::size_t effective_page_size = page_size == 0 ? remaining : static_cast<std::size_t>(page_size);
  const std::size_t result_size = std::min(effective_page_size, remaining);

  lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
  auto* configs = response.mutable_configs();
  if (start == 0 && std::cmp_equal(result_size, source_config_count)) {
    configs->MergeFrom(source_configs);
  } else {
    configs->Reserve(static_cast<int>(result_size));
    const std::size_t end = start + result_size;
    for (std::size_t index = start; index < end; ++index) {
      *configs->Add() = source_configs.Get(static_cast<int>(index));
    }
  }

  if (result_size < remaining) {
    response.set_next_page_token(std::to_string(start + result_size));
  }
  return response;
}

core::Result<void> InMemoryPushNotificationStore::Delete(std::string_view task_id, std::string_view config_id) {
  const auto validation = ValidateLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
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
  auto* configs = task_configs.list_response.mutable_configs();
  const int last_index = configs->size() - 1;
  for (int index = config_index; index < last_index; ++index) {
    configs->SwapElements(index, index + 1);
    task_configs.config_indices[configs->Get(index).id()] = index;
  }
  configs->RemoveLast();
  task_configs.config_indices.erase(config_it);

  if (task_configs.config_indices.empty()) {
    configs_.erase(task_it);
  }
  return {};
}

}  // namespace a2a::server
