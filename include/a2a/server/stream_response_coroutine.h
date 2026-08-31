// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <optional>
#include <utility>

#include "a2a/core/non_copyable.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class StreamResponseCoroutine final : private core::NonCopyable {
 public:
  struct promise_type final {
    [[nodiscard]] StreamResponseCoroutine get_return_object() {
      return StreamResponseCoroutine(std::coroutine_handle<promise_type>::from_promise(*this));
    }
    [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
    struct FinalAwaiter final {
      [[nodiscard]] bool await_ready() const noexcept { return false; }
      void await_suspend(std::coroutine_handle<promise_type> handle) const noexcept {
        auto& promise = handle.promise();
        {
          std::lock_guard lock(promise.mutex_);
          promise.done_.store(true);
        }
        promise.ready_.notify_all();
      }
      void await_resume() const noexcept {}
    };

    [[nodiscard]] FinalAwaiter final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() { exception_ = std::current_exception(); }
    [[nodiscard]] std::suspend_always yield_value(lf::a2a::v1::StreamResponse value) noexcept {
      {
        std::lock_guard lock(mutex_);
        current_value_ = std::move(value);
      }
      ready_.notify_one();
      return {};
    }

    std::optional<lf::a2a::v1::StreamResponse> current_value_;
    std::exception_ptr exception_;
    std::mutex mutex_;
    // Serializes external resume calls. Coroutine execution never acquires
    // this mutex, so a resumer may hold it until resume() returns.
    std::mutex resume_mutex_;
    std::condition_variable ready_;
    std::atomic_bool done_ = false;
  };

  StreamResponseCoroutine() = default;
  StreamResponseCoroutine(StreamResponseCoroutine&& other) noexcept
      : handle_(std::exchange(other.handle_, {})), started_(std::exchange(other.started_, false)) {}
  StreamResponseCoroutine& operator=(StreamResponseCoroutine&& other) noexcept {
    if (this != &other) {
      Destroy();
      handle_ = std::exchange(other.handle_, {});
      started_ = std::exchange(other.started_, false);
    }
    return *this;
  }

  ~StreamResponseCoroutine() { Destroy(); }

  [[nodiscard]] std::optional<lf::a2a::v1::StreamResponse> Next();
  [[nodiscard]] std::optional<lf::a2a::v1::StreamResponse> NextFor(std::chrono::milliseconds timeout);

  [[nodiscard]] bool IsDone() const noexcept { return !handle_ || handle_.promise().done_.load(); }

 private:
  explicit StreamResponseCoroutine(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

  void Destroy() noexcept {
    if (handle_) {
      handle_.destroy();
      handle_ = {};
    }
  }

  [[nodiscard]] std::optional<lf::a2a::v1::StreamResponse> WaitForNext(
      std::optional<std::chrono::milliseconds> timeout);
  void Start() noexcept;

  std::coroutine_handle<promise_type> handle_;
  bool started_ = false;
};

}  // namespace a2a::server
