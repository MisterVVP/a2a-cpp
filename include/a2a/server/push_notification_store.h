// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <functional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class TaskStore;

class PushNotificationStore {
 public:
  virtual ~PushNotificationStore() = default;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdate(
      const lf::a2a::v1::TaskPushNotificationConfig& config) = 0;
  // Creates only when the task exists. Backends that share storage with the
  // task store can override this to perform the check and write atomically.
  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdateForTask(
      const lf::a2a::v1::TaskPushNotificationConfig& config, const TaskStore& task_store);
  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> Get(std::string_view task_id,
                                                                                  std::string_view config_id) const = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetForTask(
      std::string_view task_id, std::string_view config_id, const TaskStore& task_store) const;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> List(
      std::string_view task_id, int page_size = 0, std::string_view page_token = {}) const = 0;
  // The caller must validate task existence against its authoritative task
  // store before using this method.
  [[nodiscard]] virtual core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListForExistingTask(
      std::string_view task_id, int page_size = 0, std::string_view page_token = {}) const {
    return List(task_id, page_size, page_token);
  }
  [[nodiscard]] virtual core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListForTask(
      std::string_view task_id, int page_size, std::string_view page_token, const TaskStore& task_store) const;
  [[nodiscard]] virtual core::Result<void> Delete(std::string_view task_id, std::string_view config_id) = 0;
};

struct TransparentStringHash final {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
    return (*this)(std::string_view(value));
  }

  [[nodiscard]] std::size_t operator()(const char* value) const noexcept { return (*this)(std::string_view(value)); }
};

struct TransparentStringEqual final {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
};

class InMemoryPushNotificationStore final : public PushNotificationStore {
 public:
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdate(
      const lf::a2a::v1::TaskPushNotificationConfig& config) override;
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> Get(std::string_view task_id,
                                                                          std::string_view config_id) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> List(
      std::string_view task_id, int page_size = 0, std::string_view page_token = {}) const override;
  [[nodiscard]] core::Result<void> Delete(std::string_view task_id, std::string_view config_id) override;

 private:
  using ListResponse = lf::a2a::v1::ListTaskPushNotificationConfigsResponse;

  struct TaskConfigs final {
    ListResponse list_response;
    std::unordered_map<std::string, int, TransparentStringHash, TransparentStringEqual> config_indices;
  };

  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, TaskConfigs, TransparentStringHash, TransparentStringEqual> configs_;
};

}  // namespace a2a::server
