// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <memory>

#include "a2a/core/non_copyable.h"
#include "a2a/server/engine/async_dispatcher.h"

namespace a2a::server::engine {
struct AsyncRuntimeOptions final {
  static constexpr std::size_t kDefaultThreads = 2;
  std::size_t threads = kDefaultThreads;
};

// Optional runtime owner for applications that do not already run Asio.
// Shutdown requests logical cancellation without waiting. Join/destruction drain
// coroutine cleanup; blocking backends must eventually return. Never call Join
// or destroy the runtime from its callbacks. Keep transports and dispatcher
// handles within this lifetime, and stop transport ingress before Join.
// Existing caller-supplied executors remain supported by AsyncDispatcher.
class AsyncRuntime final : private core::NonCopyableOrMovable {
 public:
  explicit AsyncRuntime(std::shared_ptr<CoroutineAgentExecutor> executor, AsyncDispatcherOptions execution = {},
                        AsyncRuntimeOptions runtime = {});
  ~AsyncRuntime();
  [[nodiscard]] std::shared_ptr<AsyncDispatcher> dispatcher() const;
  void Shutdown();
  void Join();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace a2a::server::engine
