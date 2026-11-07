// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/async_operation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <deque>
#include <exception>
#include <map>
#include <thread>
#include <unordered_map>
#include <vector>

namespace a2a::core {
namespace {
constexpr std::size_t kCompletionWorkers = 4;
constexpr std::size_t kMaximumOperations = 4096;
constexpr const char* kCancelledMessage = "async call cancelled";
constexpr const char* kShutdown = "async client shut down";
constexpr const char* kDeadline = "async call deadline exceeded";
constexpr const char* kRuntimeUnavailable = "async completion runtime is unavailable";
constexpr const char* kOverloaded = "async operation capacity exceeded";
constexpr auto kCancellationCode = AsyncHandle::kCancelled;
constexpr auto kDeadlineCode = AsyncHandle::kDeadlineExceeded;
thread_local bool g_completion_worker = false;
thread_local std::size_t g_callback_depth = 0;
using Clock = std::chrono::steady_clock;

void InvokeCallback(const std::function<void()>& callback) noexcept {
  try {
    callback();
  } catch (...) {
    // Contain application/cancellation exceptions while preserving cleanup.
    return;
  }
}

class CompletionRuntime final {
 public:
  CompletionRuntime() = default;
  void StartThreads() {
    try {
      for (auto& worker : workers_) {
        worker = std::thread([this] { Run(); });
      }
      timer_ = std::thread([this] { RunTimer(); });
    } catch (...) {
      Stop();
      throw;
    }
  }
  ~CompletionRuntime() { Stop(); }
  CompletionRuntime(const CompletionRuntime&) = delete;
  CompletionRuntime& operator=(const CompletionRuntime&) = delete;

  bool Admit(const std::shared_ptr<async_detail::Operation>& operation, std::chrono::milliseconds timeout) {
    std::call_once(started_, [this] {
      try {
        StartThreads();
      } catch (...) {
        startup_error_ = std::current_exception();
      }
    });
    if (startup_error_) {
      std::rethrow_exception(startup_error_);
    }
    std::lock_guard lock(mutex_);
    if (operations_.size() >= kMaximumOperations) {
      return false;
    }
    const auto now = Clock::now();
    const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
    const auto expires =
        timeout >= maximum ? Clock::time_point::max() : now + std::max(timeout, std::chrono::milliseconds::zero());
    auto deadline = deadlines_.emplace(expires, operation);
    operations_.emplace(operation.get(), deadline);
    deadlines_changed_.notify_one();
    return true;
  }
  void Submit(std::function<void()> task) {
    {
      std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    tasks_available_.notify_one();
  }
  void Release(async_detail::Operation* operation) {
    std::lock_guard lock(mutex_);
    const auto found = operations_.find(operation);
    if (found != operations_.end()) {
      deadlines_.erase(found->second);
      operations_.erase(found);
    }
    deadlines_changed_.notify_one();
  }

 private:
  void Stop() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    tasks_available_.notify_all();
    deadlines_changed_.notify_all();
    if (timer_.joinable()) {
      timer_.join();
    }
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }
  void Run() {
    g_completion_worker = true;
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        tasks_available_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
        if (tasks_.empty() && stopping_) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      task();
    }
  }
  void RunTimer() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      if (deadlines_.empty()) {
        deadlines_changed_.wait(lock);
      } else if (deadlines_.begin()->first > Clock::now()) {
        const auto deadline = deadlines_.begin()->first;
        deadlines_changed_.wait_until(lock, deadline);
      } else {
        auto node = deadlines_.extract(deadlines_.begin());
        auto operation = std::move(node.mapped());
        // Keep an inert deadline entry until the callback releases admission.
        auto deadline = deadlines_.emplace(Clock::time_point::max(), operation);
        operations_[operation.get()] = deadline;
        lock.unlock();
        operation->Cancel(Error::Network(kDeadline).WithProtocolCode(kDeadlineCode));
        lock.lock();
      }
    }
  }
  std::once_flag started_;
  std::exception_ptr startup_error_;
  std::mutex mutex_;
  std::condition_variable tasks_available_;
  std::condition_variable deadlines_changed_;
  std::deque<std::function<void()>> tasks_;
  std::multimap<Clock::time_point, std::shared_ptr<async_detail::Operation>> deadlines_;
  std::unordered_map<async_detail::Operation*, decltype(deadlines_)::iterator> operations_;
  std::array<std::thread, kCompletionWorkers> workers_;
  std::thread timer_;
  bool stopping_ = false;
};
CompletionRuntime& Runtime() {
  static CompletionRuntime runtime;
  return runtime;
}
}  // namespace

namespace async_detail {
void EnterCallback() noexcept { ++g_callback_depth; }
void LeaveCallback() noexcept { --g_callback_depth; }
bool IsCallbackThread() noexcept { return g_completion_worker || g_callback_depth != 0U; }

struct ScopeState final {
  std::mutex mutex;
  std::condition_variable finished;
  std::unordered_map<Operation*, std::shared_ptr<Operation>> operations;
  bool closed = false;
};

bool Operation::Claim() {
  std::lock_guard lock(mutex_);
  if (!active_) {
    return false;
  }
  active_ = false;
  cancel_ = {};
  return true;
}
void Operation::Dispatch(std::function<void()> callback) {
  Runtime().Submit([self = shared_from_this(), callback = std::move(callback)] {
    InvokeCallback(callback);
    if (auto scope = self->owner.lock()) {
      {
        std::lock_guard lock(scope->mutex);
        scope->operations.erase(self.get());
      }
      scope->finished.notify_all();
    }
    Runtime().Release(self.get());
    {
      std::lock_guard lock(self->mutex_);
      self->callback_finished_ = true;
    }
    self->finished_.notify_all();
  });
}
void Operation::Cancel(Error error) {
  std::function<void()> cancel;
  {
    std::lock_guard lock(mutex_);
    if (!active_ || cancelled_) {
      return;
    }
    cancelled_ = true;
    cancel = std::move(cancel_);
  }
  // Fail competes with successful completion through the same once-only claim.
  Fail(std::move(error));
  if (cancel) {
    InvokeCallback(cancel);
  }
}
void Operation::RegisterCancellation(const std::function<void()>& cancel) {
  bool invoke = false;
  {
    std::lock_guard lock(mutex_);
    invoke = cancelled_;
    if (active_ && !invoke) {
      cancel_ = cancel;
    }
  }
  if (invoke && cancel) {
    InvokeCallback(cancel);
  }
}
bool Operation::IsActive() const {
  std::lock_guard lock(mutex_);
  return active_;
}
void Operation::Wait() const {
  if (IsCallbackThread()) {
    return;
  }
  std::unique_lock lock(mutex_);
  finished_.wait(lock, [this] { return callback_finished_; });
}
}  // namespace async_detail

void AsyncHandle::Cancel() const {
  if (state_) {
    state_->Cancel(Error::Network(kCancelledMessage).WithProtocolCode(kCancellationCode));
  }
}
AsyncScope::AsyncScope() : state_(std::make_shared<async_detail::ScopeState>()) { (void)Runtime(); }
AsyncScope::~AsyncScope() { Shutdown(); }
Result<void> AsyncScope::Admit(const std::shared_ptr<async_detail::Operation>& operation,
                               std::chrono::milliseconds timeout) const {
  std::lock_guard lock(state_->mutex);
  if (state_->closed) {
    return Error::Network(kShutdown).WithProtocolCode(kCancellationCode);
  }
  operation->owner = state_;
  state_->operations.emplace(operation.get(), operation);
  try {
    if (!Runtime().Admit(operation, timeout)) {
      state_->operations.erase(operation.get());
      return Error::Network(kOverloaded);
    }
  } catch (...) {
    state_->operations.erase(operation.get());
    return Error::Internal(kRuntimeUnavailable);
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    operation->Cancel(Error::Network(kDeadline).WithProtocolCode(kDeadlineCode));
  }
  return {};
}
void AsyncScope::Shutdown() const {
  if (!state_) {
    return;
  }
  std::vector<std::shared_ptr<async_detail::Operation>> operations;
  {
    std::lock_guard lock(state_->mutex);
    state_->closed = true;
    operations.reserve(state_->operations.size());
    for (const auto& [key, operation] : state_->operations) {
      (void)key;
      operations.push_back(operation);
    }
  }
  for (const auto& operation : operations) {
    operation->Cancel(Error::Network(kShutdown).WithProtocolCode(kCancellationCode));
  }
  if (!async_detail::IsCallbackThread()) {
    std::unique_lock lock(state_->mutex);
    state_->finished.wait(lock, [this] { return state_->operations.empty(); });
  }
}
}  // namespace a2a::core
