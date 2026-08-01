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

class PushNotificationStore {
 public:
  virtual ~PushNotificationStore() = default;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdate(
      const lf::a2a::v1::TaskPushNotificationConfig& config) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> Get(std::string_view task_id,
                                                                                  std::string_view config_id) const = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> List(
      std::string_view task_id, int page_size = 0, std::string_view page_token = {}) const = 0;
  [[nodiscard]] virtual core::Result<void> Delete(std::string_view task_id, std::string_view config_id) = 0;

  [[nodiscard]] virtual bool ListValidatesTaskExistence() const noexcept { return false; }
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
