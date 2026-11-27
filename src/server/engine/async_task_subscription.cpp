// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_task_subscription.h"

#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <mutex>
#include <stop_token>
#include <utility>

#include "a2a/server/task_subscription_service.h"

namespace a2a::server::engine {
namespace {
constexpr auto kInvalidSubscription = "Async subscription requires a sink and positive capacity";
constexpr auto kCancelledSubscription = "Async task subscription cancelled";
using Notification = boost::asio::experimental::concurrent_channel<void(boost::system::error_code, bool)>;
}  // namespace

boost::asio::awaitable<core::Result<void>> AsyncTaskSubscription::Run(TaskSubscriptionService& service,
                                                                      lf::a2a::v1::Task task,
                                                                      RequestExecutionContext context,
                                                                      std::shared_ptr<AsyncServerStreamSession> stream,
                                                                      std::size_t capacity) {
  if (!stream || capacity == 0) {
    co_return core::Error::Validation(kInvalidSubscription);
  }
  if (context.StopToken().stop_requested()) {
    co_return core::Error::Network(kCancelledSubscription);
  }
  auto notification = std::make_shared<Notification>(co_await boost::asio::this_coro::executor, 1);
  std::stop_callback stopped(context.StopToken(),
                             [notification] { (void)notification->try_send(boost::system::error_code{}, true); });
  auto registered = service.RegisterSubscriber(
      task, capacity, [notification] { (void)notification->try_send(boost::system::error_code{}, true); });
  if (!registered.ok()) {
    co_return registered.error();
  }
  auto state = std::move(registered.value());
  struct Registration final {
    std::shared_ptr<TaskSubscriptionService::ServiceState> service;
    std::shared_ptr<TaskSubscriptionService::SubscriberState> subscriber;
    ~Registration() { TaskSubscriptionService::RemoveSubscriber(service, subscriber); }
  } registration{.service = service.state_, .subscriber = state};

  auto initial = co_await stream->Write(TaskSubscriptionService::BuildCurrentTaskEvent(state->current_task));
  if (!initial.ok()) {
    co_return initial.error();
  }
  while (true) {
    if (context.StopToken().stop_requested()) {
      co_return core::Error::Network(kCancelledSubscription);
    }
    std::shared_ptr<const lf::a2a::v1::StreamResponse> event;
    bool closed = false;
    {
      std::lock_guard lock(state->mutex);
      const auto failure = state->failure;
      if (failure.has_value()) {
        co_return failure.value();
      }
      if (!state->events.empty()) {
        event = std::move(state->events.front());
        state->events.pop_front();
      }
      closed = state->closed.load();
    }
    if (event) {
      auto result = co_await stream->Write(*event);
      if (!result.ok()) {
        co_return result.error();
      }
    } else if (closed) {
      co_return core::Result<void>{};
    } else {
      (void)co_await notification->async_receive(boost::asio::use_awaitable);
    }
  }
}
}  // namespace a2a::server::engine
