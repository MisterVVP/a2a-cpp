// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include <chrono>

#include "core/subscription_diagnostics.h"
#endif

namespace a2a::http::detail {

inline constexpr std::size_t kMaximumPendingDispatchTasks = 256U;
inline constexpr std::size_t kMaximumPendingDispatchBytes = std::size_t{4U} * 1024U * 1024U;
inline constexpr std::size_t kStreamDispatchBatchSize = 16U;

class SerialStreamDispatch final : public std::enable_shared_from_this<SerialStreamDispatch> {
 public:
  using Task = std::function<void()>;

  explicit SerialStreamDispatch(std::function<void(Task)> submit) : submit_(std::move(submit)) {}

  bool Enqueue(Task task, std::size_t pending_bytes = 0U, bool terminal = false) {
    bool schedule = false;
    {
      std::lock_guard lock(mutex_);
      const std::size_t bounded_pending_bytes =
          pending_bytes < kMaximumPendingDispatchBytes ? pending_bytes : kMaximumPendingDispatchBytes;
      if (!terminal && (tasks_.size() >= kMaximumPendingDispatchTasks ||
                        pending_bytes_ > kMaximumPendingDispatchBytes - bounded_pending_bytes)) {
        return false;
      }
      QueuedTask queued{.task = std::move(task), .pending_bytes = pending_bytes};
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
      if (core::subscription_diagnostics::IsEnabled()) {
        queued.queued_at = std::chrono::steady_clock::now();
      }
#endif
      tasks_.push_back(std::move(queued));
      pending_bytes_ += pending_bytes;
      schedule = !scheduled_;
      scheduled_ = true;
    }
    if (schedule) {
      Schedule();
    }
    return true;
  }

 private:
  void Schedule() {
    submit_([self = shared_from_this()] { self->Drain(); });
  }

  void Drain() {
    for (std::size_t count = 0; count < kStreamDispatchBatchSize; ++count) {
      QueuedTask queued;
      {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) {
          scheduled_ = false;
          return;
        }
        queued = std::move(tasks_.front());
        tasks_.pop_front();
        pending_bytes_ -= queued.pending_bytes;
      }
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
      if (queued.queued_at != std::chrono::steady_clock::time_point{}) {
        core::subscription_diagnostics::Record(core::subscription_diagnostics::Phase::kClientCallbackQueueWait,
                                               std::chrono::steady_clock::now() - queued.queued_at);
      }
#endif
      queued.task();
    }
    {
      std::lock_guard lock(mutex_);
      if (tasks_.empty()) {
        scheduled_ = false;
        return;
      }
    }
    // Keep scheduled_ set so enqueueing cannot start a concurrent drain.
    Schedule();
  }

  struct QueuedTask final {
    Task task;
    std::size_t pending_bytes = 0U;
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    std::chrono::steady_clock::time_point queued_at{};
#endif
  };

  const std::function<void(Task)> submit_;
  std::mutex mutex_;
  std::deque<QueuedTask> tasks_;
  std::size_t pending_bytes_ = 0U;
  bool scheduled_ = false;
};

}  // namespace a2a::http::detail
