// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "a2a/core/result.h"

namespace a2a::test {
inline constexpr std::size_t kAsyncIoWorkers = 2;
// A controllable foreign backend. It intentionally ignores Asio cancellation so
// tests can finish I/O after the request's logical cancellation and destruction.
template <typename T>
class CoroutineReply final {
 public:
  explicit CoroutineReply(bool ignore_cancellation = false) : ignore_cancellation_(ignore_cancellation) {}
  void Resolve(core::Result<T> result) {
    std::lock_guard lock(mutex_);
    if (result_.has_value()) {
      return;
    }
    result_ = std::move(result);
    if (const auto timer = timer_.lock()) {
      boost::asio::post(timer->get_executor(), [timer] { timer->cancel(); });
    }
  }
  boost::asio::awaitable<core::Result<T>> Wait() {
    if (ignore_cancellation_) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
    }
    auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor);
    timer->expires_at(boost::asio::steady_timer::time_point::max());
    {
      std::lock_guard lock(mutex_);
      if (result_.has_value()) {
        co_return std::move(*result_);
      }
      timer_ = timer;
    }
    boost::system::error_code error;
    co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
    std::lock_guard lock(mutex_);
    if (result_.has_value()) {
      co_return std::move(*result_);
    }
    co_return core::Error::Network(kCancelled);
  }

 private:
  static constexpr auto kCancelled = "Controlled backend operation cancelled";
  const bool ignore_cancellation_;
  std::mutex mutex_;
  std::optional<core::Result<T>> result_;
  std::weak_ptr<boost::asio::steady_timer> timer_;
};
}  // namespace a2a::test
