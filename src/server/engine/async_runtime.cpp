// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_runtime.h"

#include <boost/asio/thread_pool.hpp>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace a2a::server::engine {
namespace {
constexpr auto kInvalidThreads = "Async runtime thread count must be positive";
constexpr auto kJoinFromWorker = "Join must run outside async runtime callbacks";
std::size_t Validate(const AsyncRuntimeOptions& options) {
  if (options.threads == 0) {
    throw std::invalid_argument(kInvalidThreads);
  }
  return options.threads;
}
}  // namespace
struct AsyncRuntime::Impl final {
  Impl(std::shared_ptr<CoroutineAgentExecutor> executor, AsyncDispatcherOptions execution, AsyncRuntimeOptions runtime)
      : pool(Validate(runtime)),
        dispatcher(std::make_shared<AsyncDispatcher>(std::move(executor), pool.get_executor(), std::move(execution))) {}
  boost::asio::thread_pool pool;
  const std::shared_ptr<AsyncDispatcher> dispatcher;
  std::mutex join_mutex;
};
AsyncRuntime::AsyncRuntime(std::shared_ptr<CoroutineAgentExecutor> executor, AsyncDispatcherOptions execution,
                           AsyncRuntimeOptions runtime)
    : impl_(std::make_unique<Impl>(std::move(executor), std::move(execution), runtime)) {}
AsyncRuntime::~AsyncRuntime() {
  try {
    Join();
  } catch (...) {
    std::terminate();
  }
}
std::shared_ptr<AsyncDispatcher> AsyncRuntime::dispatcher() const { return impl_->dispatcher; }
void AsyncRuntime::Shutdown() { impl_->dispatcher->Shutdown(); }
void AsyncRuntime::Join() {
  if (impl_->pool.get_executor().running_in_this_thread()) {
    throw std::logic_error(kJoinFromWorker);
  }
  Shutdown();
  std::lock_guard lock(impl_->join_mutex);
  impl_->pool.join();
}
}  // namespace a2a::server::engine
