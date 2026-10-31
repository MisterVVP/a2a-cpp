// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#include "a2a/server/engine/async_dispatcher.h"
#include "a2a/server/engine/coroutine_agent_executor.h"

namespace a2a::test {
namespace asio = boost::asio;
inline constexpr auto kCoroutineTaskId = "coroutine-task";
inline constexpr auto kCoroutineRequestId = "coroutine-request";
inline constexpr auto kCoroutineFailure = "coroutine failure";
inline constexpr auto kCoroutineWait = std::chrono::seconds(5);
inline constexpr auto kPumpInterval = std::chrono::milliseconds(1);
inline constexpr auto kNoWait = std::chrono::seconds::zero();
inline constexpr std::size_t kCoroutineWorkers = 2;
inline constexpr std::size_t kCoroutineRequests = 16;

enum class CoroutineBehavior { kImmediate, kWait, kIgnoreCancellation, kError, kThrow };

class CoroutineBackend final : public server::engine::CoroutineAgentExecutor {
 public:
  explicit CoroutineBackend(CoroutineBehavior behavior = CoroutineBehavior::kImmediate) : behavior_(behavior) {}

  asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(lf::a2a::v1::GetTaskRequest request,
                                                           server::engine::RequestExecutionContext context) override {
    FinishObserver finished(*this);
    if (behavior_ == CoroutineBehavior::kIgnoreCancellation) {
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
    }
    auto timer = std::make_shared<asio::steady_timer>(co_await asio::this_coro::executor);
    timer->expires_at(asio::steady_timer::time_point::max());
    bool wait = false;
    {
      std::lock_guard lock(mutex_);
      timers_.push_back(timer);
      metadata_ = context.Metadata();
      wait =
          !released_ && (behavior_ == CoroutineBehavior::kWait || behavior_ == CoroutineBehavior::kIgnoreCancellation);
      ++started;
    }
    changed_.notify_all();
    if (wait) {
      boost::system::error_code error;
      co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
    }
    if (behavior_ == CoroutineBehavior::kThrow) {
      throw std::runtime_error(kCoroutineFailure);
    }
    if (behavior_ == CoroutineBehavior::kError) {
      co_return core::Error::Validation(kCoroutineFailure);
    }
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    co_return task;
  }
  asio::awaitable<core::Result<lf::a2a::v1::SendMessageResponse>> SendMessage(
      lf::a2a::v1::SendMessageRequest /*request*/, server::engine::RequestExecutionContext /*context*/) override {
    co_return lf::a2a::v1::SendMessageResponse{};
  }
  asio::awaitable<core::Result<server::ListTasksResponse>> ListTasks(
      server::ListTasksRequest /*request*/, server::engine::RequestExecutionContext /*context*/) override {
    co_return server::ListTasksResponse{};
  }
  asio::awaitable<core::Result<lf::a2a::v1::Task>> CancelTask(
      lf::a2a::v1::CancelTaskRequest /*request*/, server::engine::RequestExecutionContext /*context*/) override {
    co_return lf::a2a::v1::Task{};
  }
  asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> CreateTaskPushNotificationConfig(
      lf::a2a::v1::TaskPushNotificationConfig /*request*/,
      server::engine::RequestExecutionContext /*context*/) override {
    co_return lf::a2a::v1::TaskPushNotificationConfig{};
  }
  asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> GetTaskPushNotificationConfig(
      lf::a2a::v1::GetTaskPushNotificationConfigRequest /*request*/,
      server::engine::RequestExecutionContext /*context*/) override {
    co_return lf::a2a::v1::TaskPushNotificationConfig{};
  }
  asio::awaitable<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>> ListTaskPushNotificationConfigs(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest /*request*/,
      server::engine::RequestExecutionContext /*context*/) override {
    co_return lf::a2a::v1::ListTaskPushNotificationConfigsResponse{};
  }
  asio::awaitable<core::Result<void>> DeleteTaskPushNotificationConfig(
      lf::a2a::v1::DeleteTaskPushNotificationConfigRequest /*request*/,
      server::engine::RequestExecutionContext /*context*/) override {
    co_return core::Result<void>{};
  }
  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    for (const auto& pending : timers_) {
      if (const auto timer = pending.lock()) {
        asio::post(timer->get_executor(), [timer] { timer->cancel(); });
      }
    }
  }
  bool WaitStarted(std::size_t count = 1) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kCoroutineWait, [&] { return started >= count; });
  }
  bool WaitFinished(std::size_t count = 1) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kCoroutineWait, [&] { return completed >= count; });
  }
  server::RequestContext Metadata() {
    std::lock_guard lock(mutex_);
    return metadata_;
  }
  std::atomic_size_t started = 0;
  std::atomic_size_t completed = 0;

 private:
  struct FinishObserver final {
    explicit FinishObserver(CoroutineBackend& backend) : backend(backend) {}
    ~FinishObserver() {
      {
        std::lock_guard lock(backend.mutex_);
        ++backend.completed;
      }
      backend.changed_.notify_all();
    }
    CoroutineBackend& backend;
  };
  const CoroutineBehavior behavior_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::weak_ptr<asio::steady_timer>> timers_;
  server::RequestContext metadata_;
  bool released_ = false;
};

template <typename Predicate>
bool PumpUntil(asio::io_context& loop, Predicate predicate) {
  const auto limit = std::chrono::steady_clock::now() + kCoroutineWait;
  while (!predicate() && std::chrono::steady_clock::now() < limit) {
    loop.restart();
    loop.run_for(kPumpInterval);
  }
  return predicate();
}

template <typename T>
bool PumpReady(asio::io_context& loop, std::future<T>& result) {
  return PumpUntil(loop, [&] { return result.wait_for(kNoWait) == std::future_status::ready; });
}

inline server::DispatchRequest CoroutineRequest() {
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kCoroutineTaskId);
  return {.operation = server::DispatcherOperation::kGetTask, .payload = std::move(request)};
}

inline std::vector<server::DispatchRequest> CoroutineUnaryRequests() {
  using server::DispatcherOperation;
  return {
      {.operation = DispatcherOperation::kSendMessage, .payload = lf::a2a::v1::SendMessageRequest{}},
      {.operation = DispatcherOperation::kGetTask, .payload = lf::a2a::v1::GetTaskRequest{}},
      {.operation = DispatcherOperation::kListTasks, .payload = server::ListTasksRequest{}},
      {.operation = DispatcherOperation::kCancelTask, .payload = lf::a2a::v1::CancelTaskRequest{}},
      {.operation = DispatcherOperation::kCreateTaskPushNotificationConfig,
       .payload = lf::a2a::v1::TaskPushNotificationConfig{}},
      {.operation = DispatcherOperation::kGetTaskPushNotificationConfig,
       .payload = lf::a2a::v1::GetTaskPushNotificationConfigRequest{}},
      {.operation = DispatcherOperation::kListTaskPushNotificationConfigs,
       .payload = lf::a2a::v1::ListTaskPushNotificationConfigsRequest{}},
      {.operation = DispatcherOperation::kDeleteTaskPushNotificationConfig,
       .payload = lf::a2a::v1::DeleteTaskPushNotificationConfigRequest{}},
  };
}
}  // namespace a2a::test
