// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include "stream_worker_executor.h"

#include <stdexcept>
#include <utility>
namespace a2a::client::internal {
namespace {
constexpr char kShutdownSubmissionMessage[] = "cannot submit work while the stream executor is shutting down";
}

StreamWorkerExecutor::StreamWorkerExecutor(std::size_t maximum_idle_workers)
    : maximum_idle_workers_(maximum_idle_workers) {}
StreamWorkerExecutor::~StreamWorkerExecutor() {
  {
    std::lock_guard lock(mutex_);
    shutting_down_ = true;
  }
  work_available_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}
void StreamWorkerExecutor::Submit(Task task, Task on_complete) {
  std::lock_guard lock(mutex_);
  if (shutting_down_) {
    throw std::runtime_error(kShutdownSubmissionMessage);
  }
  if (idle_workers_ != 0) {
    pending_tasks_.push_back({.task = std::move(task), .on_complete = std::move(on_complete)});
    work_available_.notify_one();
    return;
  }
  workers_.emplace_back([this, work = Work{.task = std::move(task), .on_complete = std::move(on_complete)}]() mutable {
    RunWorker(std::move(work));
  });
}
void StreamWorkerExecutor::RunWorker(Work work) {
  while (true) {
    work.task();
    std::unique_lock lock(mutex_);
    if (shutting_down_ || idle_workers_ >= maximum_idle_workers_) {
      lock.unlock();
      if (work.on_complete) {
        work.on_complete();
      }
      return;
    }
    ++idle_workers_;
    lock.unlock();
    if (work.on_complete) {
      work.on_complete();
    }
    lock.lock();
    work_available_.wait(lock, [this] { return shutting_down_ || !pending_tasks_.empty(); });
    --idle_workers_;
    if (shutting_down_) {
      return;
    }
    work = std::move(pending_tasks_.back());
    pending_tasks_.pop_back();
  }
}
}  // namespace a2a::client::internal
