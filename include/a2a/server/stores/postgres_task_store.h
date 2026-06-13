// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <mutex>
#include <string_view>

#include "a2a/server/server.h"
#include "a2a/server/stores/store_factory.h"

namespace a2a::server::stores {

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

}  // namespace a2a::server::stores
