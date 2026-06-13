// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>

#include "a2a/core/error.h"
#include "a2a/server/stores/postgres_notification_store.h"
#include "a2a/server/stores/postgres_task_store.h"

namespace a2a::server::stores {

inline constexpr std::size_t kDefaultPostgresConnectionPoolSize = 4;

#ifdef A2A_POSTGRES_STORE_TESTING
void FailNextPostgresAcquireForTesting(core::Error error);
#endif

}  // namespace a2a::server::stores
