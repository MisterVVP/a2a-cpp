// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/store_factory.h"

#include <memory>

#include "a2a/core/error.h"

namespace a2a::server::stores {

core::Result<StoreBundle> CreateInMemoryStoreBundle() {
  StoreBundle bundle;
  bundle.task_store = std::make_unique<InMemoryTaskStore>();
  bundle.push_store = std::make_unique<InMemoryPushNotificationStore>();
  return bundle;
}

#ifndef A2A_ENABLE_POSTGRES_STORE
core::Result<StoreBundle> CreatePostgresStoreBundle(const PostgresStoreOptions& options) {
  (void)options;
  return core::Error::Internal("PostgreSQL store backend was not built; rebuild with A2A_ENABLE_POSTGRES_STORE=ON");
}
#endif

}  // namespace a2a::server::stores
