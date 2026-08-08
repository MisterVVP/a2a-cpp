// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "a2a/core/non_copyable.h"
#include "a2a/core/result.h"
#include "a2a/server/push_notification_store.h"
#include "a2a/server/tasks/in_memory_task_store.h"

namespace a2a::server::stores {

constexpr std::size_t kDefaultPostgresConnectionPoolSize = 4U;

enum class StoreBackendKind {
  kInMemory,
  kPostgres,
};

struct PostgresStoreOptions final {
  std::string connection_string;
  std::string schema = "public";
  bool auto_create_schema = true;
  std::size_t connection_pool_size = kDefaultPostgresConnectionPoolSize;
  // Optional operator-provided identity for the physical PostgreSQL authority.
  // Matching non-empty IDs prove local storage; different non-empty IDs prove external storage.
  std::string storage_authority_id = {};
};

struct StoreBundle final {
  std::unique_ptr<TaskStore> task_store;
  std::unique_ptr<PushNotificationStore> push_store;
};

class StoreFactory : private core::NonCopyableOrMovable {
 public:
  StoreFactory() = default;
  virtual ~StoreFactory() = default;

  [[nodiscard]] virtual StoreBackendKind backend_kind() const noexcept = 0;
  [[nodiscard]] virtual core::Result<std::unique_ptr<TaskStore>> CreateTaskStore() const = 0;
  [[nodiscard]] virtual core::Result<std::unique_ptr<PushNotificationStore>> CreatePushNotificationStore() const = 0;
  [[nodiscard]] virtual core::Result<StoreBundle> CreateStoreBundle() const;
};

class InMemoryStoreFactory final : public StoreFactory {
 public:
  [[nodiscard]] StoreBackendKind backend_kind() const noexcept override;
  [[nodiscard]] core::Result<std::unique_ptr<TaskStore>> CreateTaskStore() const override;
  [[nodiscard]] core::Result<std::unique_ptr<PushNotificationStore>> CreatePushNotificationStore() const override;
};

class PostgresStoreFactory final : public StoreFactory {
 public:
  explicit PostgresStoreFactory(PostgresStoreOptions options);

  [[nodiscard]] StoreBackendKind backend_kind() const noexcept override;
  [[nodiscard]] core::Result<std::unique_ptr<TaskStore>> CreateTaskStore() const override;
  [[nodiscard]] core::Result<std::unique_ptr<PushNotificationStore>> CreatePushNotificationStore() const override;
  [[nodiscard]] core::Result<StoreBundle> CreateStoreBundle() const override;
  [[nodiscard]] const PostgresStoreOptions& options() const noexcept;

 private:
  PostgresStoreOptions options_;
};

}  // namespace a2a::server::stores
