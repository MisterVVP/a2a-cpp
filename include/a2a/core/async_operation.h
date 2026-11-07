// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "a2a/core/non_copyable.h"
#include "a2a/core/result.h"

namespace a2a::core {

template <typename T>
using Completion = std::function<void(Result<T>)>;

namespace async_detail {
void EnterCallback() noexcept;
void LeaveCallback() noexcept;
[[nodiscard]] bool IsCallbackThread() noexcept;
struct ScopeState;
class Operation : public std::enable_shared_from_this<Operation>, private NonCopyableOrMovable {
 public:
  virtual ~Operation() = default;
  virtual void Fail(Error error) = 0;
  void Cancel(Error error);
  void RegisterCancellation(const std::function<void()>& cancel);
  [[nodiscard]] bool IsActive() const;
  void Wait() const;
  std::weak_ptr<ScopeState> owner;

 protected:
  bool Claim();
  void Dispatch(std::function<void()> callback);

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable finished_;
  std::function<void()> cancel_;
  bool active_ = true;
  bool cancelled_ = false;
  bool callback_finished_ = false;
};

template <typename T>
class TypedOperation final : public Operation {
 public:
  explicit TypedOperation(Completion<T> callback) : callback_(std::move(callback)) {}
  void Complete(Result<T> result) {
    if (Claim()) {
      Dispatch(
          [callback = std::move(callback_), result = std::move(result)]() mutable { callback(std::move(result)); });
    }
  }
  void Fail(Error error) override { Complete(std::move(error)); }

 private:
  Completion<T> callback_;
};
}  // namespace async_detail

// Copyable cancellation/observation handle. Dropping it does not cancel a call.
// Wait waits for the callback to return; it is a no-op inside SDK callbacks.
class AsyncHandle final {
 public:
  static constexpr const char* kCancelled = "CANCELLED";
  static constexpr const char* kDeadlineExceeded = "DEADLINE_EXCEEDED";
  void Cancel() const;
  void Wait() const {
    if (state_) {
      state_->Wait();
    }
  }
  [[nodiscard]] bool IsActive() const { return state_ && state_->IsActive(); }

 private:
  template <typename T>
  friend class AsyncCompletion;
  explicit AsyncHandle(std::shared_ptr<async_detail::Operation> state) : state_(std::move(state)) {}
  std::shared_ptr<async_detail::Operation> state_;
};

// Transport-side completion token. Complete is once-only and never invokes the
// application inline. Cancellation registrars must only signal network work.
template <typename T>
class AsyncCompletion final {
 public:
  void Complete(Result<T> result) const { state_->Complete(std::move(result)); }
  void RegisterCancellation(const std::function<void()>& cancel) const { state_->RegisterCancellation(cancel); }
  [[nodiscard]] bool IsActive() const { return state_->IsActive(); }
  [[nodiscard]] AsyncHandle handle() const { return AsyncHandle(state_); }

 private:
  friend class AsyncScope;
  explicit AsyncCompletion(std::shared_ptr<async_detail::TypedOperation<T>> state) : state_(std::move(state)) {}
  std::shared_ptr<async_detail::TypedOperation<T>> state_;
};

// Owns accepted operations through callback completion. Shutdown cancels them
// and drains callbacks, except when called from an SDK callback (reentrant).
class AsyncScope final {
 public:
  AsyncScope();
  ~AsyncScope();
  AsyncScope(const AsyncScope&) = delete;
  AsyncScope& operator=(const AsyncScope&) = delete;
  AsyncScope(AsyncScope&&) = delete;
  AsyncScope& operator=(AsyncScope&&) = delete;

  template <typename T>
  [[nodiscard]] Result<AsyncCompletion<T>> Start(Completion<T> callback, std::chrono::milliseconds timeout) const {
    if (!callback) {
      return Error::Validation(kMissingCompletion);
    }
    auto operation = std::make_shared<async_detail::TypedOperation<T>>(std::move(callback));
    const auto admitted = Admit(operation, timeout);
    if (!admitted.ok()) {
      return admitted.error();
    }
    return AsyncCompletion<T>(std::move(operation));
  }
  void Shutdown() const;

 private:
  static constexpr const char* kMissingCompletion = "async completion callback is required";
  [[nodiscard]] Result<void> Admit(const std::shared_ptr<async_detail::Operation>& operation,
                                   std::chrono::milliseconds timeout) const;
  std::shared_ptr<async_detail::ScopeState> state_;
};

}  // namespace a2a::core
