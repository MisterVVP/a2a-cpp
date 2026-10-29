// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "a2a/core/non_copyable.h"

namespace a2a::tests::sut {
class SutWorkQueue final : private core::NonCopyableOrMovable {
 public:
  static constexpr std::size_t kWorkerCount = 2;
  static constexpr std::size_t kMaxPending = 1024;
  explicit SutWorkQueue(std::size_t workers = kWorkerCount, std::size_t max_pending = kMaxPending);
  ~SutWorkQueue();
  [[nodiscard]] bool Post(std::function<void()> work);
  void Shutdown();

 private:
  void Run();
  const std::size_t max_pending_;
  std::mutex mutex_;
  std::condition_variable ready_;
  bool stopped_ = false;
  std::deque<std::function<void()>> pending_;
  std::vector<std::thread> workers_;
};
}  // namespace a2a::tests::sut
