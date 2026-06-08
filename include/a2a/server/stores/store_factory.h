// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <string>

#include "a2a/core/result.h"
#include "a2a/server/push_notification_store.h"
#include "a2a/server/server.h"

namespace a2a::server::stores {

enum class StoreBackendKind {
  kInMemory,
  kPostgres,
};

struct PostgresStoreOptions final {
  std::string connection_string;
  std::string schema = "public";
  bool auto_create_schema = true;
};

struct StoreBundle final {
  std::unique_ptr<TaskStore> task_store;
  std::unique_ptr<PushNotificationStore> push_store;
};

class StoreFactory {
 public:
  StoreFactory() = default;
  StoreFactory(const StoreFactory&) = delete;
  StoreFactory& operator=(const StoreFactory&) = delete;
  StoreFactory(StoreFactory&&) = delete;
  StoreFactory& operator=(StoreFactory&&) = delete;
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
