// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/store_factory.h"

#include <memory>
#include <utility>

#include "a2a/core/error.h"

namespace a2a::server::stores {

core::Result<StoreBundle> StoreFactory::CreateStoreBundle() const {
  auto task_store = CreateTaskStore();
  if (!task_store.ok()) {
    return task_store.error();
  }
  auto push_store = CreatePushNotificationStore();
  if (!push_store.ok()) {
    return push_store.error();
  }

  StoreBundle bundle;
  bundle.task_store = std::move(task_store.value());
  bundle.push_store = std::move(push_store.value());
  return bundle;
}

StoreBackendKind InMemoryStoreFactory::backend_kind() const noexcept { return StoreBackendKind::kInMemory; }

core::Result<std::unique_ptr<TaskStore>> InMemoryStoreFactory::CreateTaskStore() const {
  return std::unique_ptr<TaskStore>(std::make_unique<InMemoryTaskStore>());
}

core::Result<std::unique_ptr<PushNotificationStore>> InMemoryStoreFactory::CreatePushNotificationStore() const {
  return std::unique_ptr<PushNotificationStore>(std::make_unique<InMemoryPushNotificationStore>());
}

#ifndef A2A_ENABLE_POSTGRES_STORE
PostgresStoreFactory::PostgresStoreFactory(PostgresStoreOptions options) : options_(std::move(options)) {}

StoreBackendKind PostgresStoreFactory::backend_kind() const noexcept { return StoreBackendKind::kPostgres; }

core::Result<std::unique_ptr<TaskStore>> PostgresStoreFactory::CreateTaskStore() const {
  return core::Error::Internal("PostgreSQL store backend was not built; rebuild with A2A_ENABLE_POSTGRES_STORE=ON");
}

core::Result<std::unique_ptr<PushNotificationStore>> PostgresStoreFactory::CreatePushNotificationStore() const {
  return core::Error::Internal("PostgreSQL store backend was not built; rebuild with A2A_ENABLE_POSTGRES_STORE=ON");
}

core::Result<StoreBundle> PostgresStoreFactory::CreateStoreBundle() const {
  return core::Error::Internal("PostgreSQL store backend was not built; rebuild with A2A_ENABLE_POSTGRES_STORE=ON");
}

const PostgresStoreOptions& PostgresStoreFactory::options() const noexcept { return options_; }

#endif

}  // namespace a2a::server::stores
