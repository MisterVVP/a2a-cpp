// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <string_view>

#include "a2a/server/push_notification_store.h"
#include "a2a/server/stores/store_factory.h"

namespace a2a::server::stores {

class PostgresConnectionPool;

class PostgresPushNotificationStore final : public a2a::server::PushNotificationStore {
 public:
  explicit PostgresPushNotificationStore(PostgresStoreOptions options);
  PostgresPushNotificationStore(std::shared_ptr<PostgresConnectionPool> pool, PostgresStoreOptions options);
  ~PostgresPushNotificationStore() override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdate(
      const lf::a2a::v1::TaskPushNotificationConfig& config) override;
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> Get(std::string_view task_id,
                                                                          std::string_view config_id) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> List(
      std::string_view task_id, int page_size = 0, std::string_view page_token = {}) const override;
  [[nodiscard]] core::Result<void> Delete(std::string_view task_id, std::string_view config_id) override;

 private:
  std::shared_ptr<PostgresConnectionPool> pool_;
  PostgresStoreOptions options_;
};

}  // namespace a2a::server::stores
