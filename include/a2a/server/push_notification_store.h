// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <mutex>
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
      std::string_view task_id) const = 0;
  [[nodiscard]] virtual core::Result<void> Delete(std::string_view task_id, std::string_view config_id) = 0;
};

class InMemoryPushNotificationStore final : public PushNotificationStore {
 public:
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdate(
      const lf::a2a::v1::TaskPushNotificationConfig& config) override;
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> Get(std::string_view task_id,
                                                                          std::string_view config_id) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> List(
      std::string_view task_id) const override;
  [[nodiscard]] core::Result<void> Delete(std::string_view task_id, std::string_view config_id) override;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::unordered_map<std::string, lf::a2a::v1::TaskPushNotificationConfig>> configs_;
};

}  // namespace a2a::server
