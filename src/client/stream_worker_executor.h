// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#pragma once
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
namespace a2a::client::internal {
inline constexpr std::size_t kMaximumRetainedIdleStreamWorkers = 64;
class StreamWorkerExecutor final {
 public:
  using Task = std::function<void()>;
  explicit StreamWorkerExecutor(std::size_t maximum_idle_workers = kMaximumRetainedIdleStreamWorkers);
  StreamWorkerExecutor(const StreamWorkerExecutor&) = delete;
  StreamWorkerExecutor& operator=(const StreamWorkerExecutor&) = delete;
  ~StreamWorkerExecutor();
  void Submit(Task task, Task on_complete = {});

 private:
  struct Work final {
    Task task;
    Task on_complete;
  };

  void RunWorker(Work work);
  const std::size_t maximum_idle_workers_;
  std::mutex mutex_;
  std::condition_variable work_available_;
  std::vector<Work> pending_tasks_;
  std::vector<std::thread> workers_;
  std::size_t idle_workers_ = 0;
  bool shutting_down_ = false;
};
}  // namespace a2a::client::internal
