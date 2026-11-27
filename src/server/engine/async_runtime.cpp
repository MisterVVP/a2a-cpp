// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_runtime.h"

#include <algorithm>
#include <boost/asio/thread_pool.hpp>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

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
  Impl(std::shared_ptr<CoroutineAgentExecutor> executor, AsyncDispatcherOptions execution,
       AsyncRuntimeOptions runtime) {
    const auto count = Validate(runtime);
    workers.reserve(count);
    std::vector<boost::asio::any_io_executor> lanes;
    lanes.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      workers.push_back(std::make_unique<boost::asio::thread_pool>(1));
      lanes.emplace_back(workers.back()->get_executor());
    }
    dispatcher = std::make_shared<AsyncDispatcher>(std::move(executor), lanes.front(), std::move(execution));
    dispatcher->EnableWorkerLanes(std::move(lanes));
  }
  std::vector<std::unique_ptr<boost::asio::thread_pool>> workers;
  std::shared_ptr<AsyncDispatcher> dispatcher;
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
  if (std::ranges::any_of(impl_->workers,
                          [](const auto& worker) { return worker->get_executor().running_in_this_thread(); })) {
    throw std::logic_error(kJoinFromWorker);
  }
  Shutdown();
  std::lock_guard lock(impl_->join_mutex);
  for (const auto& worker : impl_->workers) {
    worker->join();
  }
}
}  // namespace a2a::server::engine
