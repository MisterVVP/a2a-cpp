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

[[nodiscard]] core::Result<StoreBundle> CreateInMemoryStoreBundle();
[[nodiscard]] core::Result<StoreBundle> CreatePostgresStoreBundle(const PostgresStoreOptions& options);

}  // namespace a2a::server::stores
