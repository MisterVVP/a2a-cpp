// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/sut_work_queue.h"

#include <stdexcept>
#include <utility>

namespace a2a::tests::sut {
namespace {
constexpr auto kInvalidQueueSize = "SUT worker and queue limits must be positive";
}
SutWorkQueue::SutWorkQueue(std::size_t workers, std::size_t max_pending) : max_pending_(max_pending) {
  if (workers == 0 || max_pending == 0) {
    throw std::invalid_argument(kInvalidQueueSize);
  }
  try {
    for (std::size_t index = 0; index < workers; ++index) {
      workers_.emplace_back([this] { Run(); });
    }
  } catch (...) {
    Shutdown();
    throw;
  }
}
SutWorkQueue::~SutWorkQueue() { Shutdown(); }
bool SutWorkQueue::Post(std::function<void()> work) {
  {
    std::lock_guard lock(mutex_);
    if (stopped_ || pending_.size() >= max_pending_) {
      return false;
    }
    pending_.push_back(std::move(work));
  }
  ready_.notify_one();
  return true;
}
void SutWorkQueue::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    stopped_ = true;
  }
  ready_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}
void SutWorkQueue::Run() {
  for (;;) {
    std::function<void()> work;
    {
      std::unique_lock lock(mutex_);
      ready_.wait(lock, [this] { return stopped_ || !pending_.empty(); });
      if (pending_.empty()) {
        return;
      }
      work = std::move(pending_.front());
      pending_.pop_front();
    }
    work();
  }
}
}  // namespace a2a::tests::sut
