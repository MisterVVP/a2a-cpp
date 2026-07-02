// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <coroutine>
#include <exception>
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
    [[nodiscard]] std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() { exception_ = std::current_exception(); }
    [[nodiscard]] std::suspend_always yield_value(lf::a2a::v1::StreamResponse value) noexcept {
      current_value_ = std::move(value);
      return {};
    }

    std::optional<lf::a2a::v1::StreamResponse> current_value_;
    std::exception_ptr exception_;
  };

  StreamResponseCoroutine() = default;
  StreamResponseCoroutine(StreamResponseCoroutine&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  StreamResponseCoroutine& operator=(StreamResponseCoroutine&& other) noexcept {
    if (this != &other) {
      Destroy();
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  ~StreamResponseCoroutine() { Destroy(); }

  [[nodiscard]] std::optional<lf::a2a::v1::StreamResponse> Next() {
    if (IsDone()) {
      return std::nullopt;
    }
    handle_.promise().current_value_.reset();
    handle_.resume();
    if (handle_.promise().exception_ != nullptr) {
      std::rethrow_exception(handle_.promise().exception_);
    }
    if (handle_.done()) {
      return std::nullopt;
    }
    return std::move(handle_.promise().current_value_);
  }

  [[nodiscard]] bool IsDone() const noexcept { return !handle_ || handle_.done(); }

 private:
  explicit StreamResponseCoroutine(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

  void Destroy() noexcept {
    if (handle_) {
      handle_.destroy();
      handle_ = {};
    }
  }

  std::coroutine_handle<promise_type> handle_;
};

}  // namespace a2a::server
