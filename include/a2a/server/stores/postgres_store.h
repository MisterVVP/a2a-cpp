// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "a2a/server/push_notification_store.h"
#include "a2a/server/server.h"
#include "a2a/server/stores/store_factory.h"

namespace a2a::server::stores {

inline constexpr std::size_t kDefaultPostgresConnectionPoolSize = 4;

class PostgresConnectionPool;

class PostgresTaskStore final : public a2a::server::TaskStore {
 public:
  explicit PostgresTaskStore(PostgresStoreOptions options);
  PostgresTaskStore(std::shared_ptr<PostgresConnectionPool> pool, PostgresStoreOptions options);
  ~PostgresTaskStore() override;

  [[nodiscard]] core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override;
  [[nodiscard]] core::Result<ListTasksResponse> List(const ListTasksRequest& request) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                  const lf::a2a::v1::Message& message,
                                                                  HistoryAppendPolicy policy) override;
  [[nodiscard]] HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override;

 private:
  std::shared_ptr<PostgresConnectionPool> pool_;
  PostgresStoreOptions options_;
  mutable std::mutex telemetry_mutex_;
  HistoryTelemetrySnapshot telemetry_snapshot_;
};

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
