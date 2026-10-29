// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <memory>
#include <utility>

#include "a2a/core/result.h"
#include "sut/sut_work_queue.h"

namespace a2a::tests::sut {
inline constexpr auto kSutOperationFailed = "SUT application operation threw an exception";
inline constexpr auto kSutOperationStopped = "SUT operation cancelled or work queue unavailable";

// Blocking fixture work runs on the bounded backend queue. Its completion returns
// to the awaiting coroutine's executor, which stays alive until delivery.
template <typename Response, typename Operation>
boost::asio::awaitable<core::Result<Response>> AwaitSutWork(SutWorkQueue& queue, Operation operation) {
  auto initiate = [&queue, operation = std::move(operation)](auto handler) mutable {
    using Handler = decltype(handler);
    const auto completion = std::make_shared<Handler>(std::move(handler));
    const auto scheduler = boost::asio::get_associated_executor(*completion);
    const auto work =
        std::make_shared<decltype(boost::asio::make_work_guard(scheduler))>(boost::asio::make_work_guard(scheduler));
    auto deliver = [completion, scheduler, work](core::Result<Response> result) {
      boost::asio::post(scheduler,
                        [completion, work, result = std::move(result)]() mutable { (*completion)(std::move(result)); });
    };
    const bool accepted = queue.Post([operation = std::move(operation), deliver]() mutable {
      core::Result<Response> result = core::Error::Internal(kSutOperationFailed);
      try {
        result = operation();
      } catch (...) {
        result = core::Error::Internal(kSutOperationFailed);
      }
      deliver(std::move(result));
    });
    if (!accepted) {
      deliver(core::Error::Network(kSutOperationStopped));
    }
  };
  co_return co_await boost::asio::async_initiate<decltype(boost::asio::use_awaitable), void(core::Result<Response>)>(
      std::move(initiate), boost::asio::use_awaitable);
}
}  // namespace a2a::tests::sut
