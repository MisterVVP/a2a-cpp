// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <memory>

#include "a2a/server/engine/async_server_stream_session.h"
#include "a2a/server/engine/request_execution.h"

namespace a2a::server {
class TaskSubscriptionService;
namespace engine {
class AsyncTaskSubscription final {
 public:
  static constexpr std::size_t kDefaultCapacity = 64;
  // Shares snapshot, publication order and terminal semantics with Subscribe().
  // Buffer overflow fails this subscriber; publishers and other consumers proceed.
  static boost::asio::awaitable<core::Result<void>> Run(TaskSubscriptionService& service, lf::a2a::v1::Task task,
                                                        RequestExecutionContext context,
                                                        std::shared_ptr<AsyncServerStreamSession> stream,
                                                        std::size_t capacity = kDefaultCapacity);
};
}  // namespace engine
}  // namespace a2a::server
