// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_push_notification_service.h"

#include <boost/asio/error.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <optional>
#include <stop_token>
#include <utility>

#include "a2a/core/http_constants.h"
#include "a2a/core/non_copyable.h"
#include "a2a/server/push_notification_service.h"

namespace a2a::server::engine {
namespace {
constexpr auto kMissingClient = "async push delivery client is not configured";
constexpr auto kCancelled = "async push delivery cancelled";
using DeliveryResult = core::Result<PushDeliveryResult>;
using Notification =
    boost::asio::experimental::concurrent_channel<void(boost::system::error_code, std::optional<DeliveryResult>)>;
class CancelDeliveryOnExit final : private core::NonCopyableOrMovable {
 public:
  explicit CancelDeliveryOnExit(core::AsyncHandle handle) : handle_(std::move(handle)) {}
  ~CancelDeliveryOnExit() {
    if (handle_.IsActive()) {
      handle_.Cancel();
    }
  }

 private:
  core::AsyncHandle handle_;
};

core::Error Cancelled() { return core::Error::Network(kCancelled).WithProtocolCode(core::AsyncHandle::kCancelled); }
core::Result<void> CheckDelivery(const PushDeliveryResult& result) {
  if (!result.error_message.empty()) {
    return core::Error::RemoteProtocol(result.error_message).WithHttpStatus(result.http_status);
  }
  if (result.http_status < core::http::kSuccessStatusMin || result.http_status > core::http::kSuccessStatusMax) {
    return core::Error::RemoteProtocol(std::string(PushNotificationService::kPushDeliveryNonSuccessStatusMessage))
        .WithHttpStatus(result.http_status);
  }
  return {};
}
}  // namespace

boost::asio::awaitable<core::Result<PushDeliveryResult>> AsyncPushNotificationService::Deliver(
    std::shared_ptr<AsyncPushNotificationDeliveryClient> client, PushDeliveryRequest request,
    RequestExecutionContext context, AsyncPushDeliveryOptions options) {
  if (!client) {
    co_return core::Error::Validation(kMissingClient);
  }
  if (context.StopToken().stop_requested()) {
    co_return Cancelled();
  }
  const auto deadline = context.Deadline();
  if (deadline && (!options.deadline || *deadline < *options.deadline)) {
    options.deadline = deadline;
  }
  auto notification = std::make_shared<Notification>(co_await boost::asio::this_coro::executor, 1);
  auto started = client->DeliverAsync(
      request,
      [notification](DeliveryResult result) {
        (void)notification->try_send(boost::system::error_code{}, std::optional<DeliveryResult>(std::move(result)));
      },
      options);
  if (!started.ok()) {
    co_return started.error();
  }
  auto handle = std::move(started.value());
  CancelDeliveryOnExit cancel_delivery(handle);
  std::stop_callback stopped(context.StopToken(), [handle] { handle.Cancel(); });
  try {
    boost::system::error_code error;
    auto result = co_await notification->async_receive(boost::asio::redirect_error(boost::asio::use_awaitable, error));
    if (error || !result.has_value()) {
      co_return Cancelled();
    }
    co_return std::move(*result);
  } catch (const boost::system::system_error& error) {
    if (error.code() == boost::asio::error::operation_aborted) {
      co_return Cancelled();
    }
    throw;
  }
}

boost::asio::awaitable<core::Result<void>> AsyncPushNotificationService::NotifyTaskUpdated(
    PushNotificationStore& store, std::shared_ptr<AsyncPushNotificationDeliveryClient> client, lf::a2a::v1::Task task,
    RequestExecutionContext context, AsyncPushDeliveryOptions options) {
  if (!client) {
    co_return core::Error::Validation(kMissingClient);
  }
  if (context.StopToken().stop_requested()) {
    co_return Cancelled();
  }
  auto configs = store.ListForExistingTask(task.id());
  if (!configs.ok()) {
    co_return configs.error();
  }
  const auto payload = BuildTaskStatusUpdatePayload(task);
  for (const auto& config : configs.value().configs()) {
    auto result = co_await Deliver(client, PushDeliveryRequest{.config = config, .payload = payload}, context, options);
    if (!result.ok()) {
      co_return result.error();
    }
    auto checked = CheckDelivery(result.value());
    if (!checked.ok()) {
      co_return checked.error();
    }
  }
  co_return core::Result<void>{};
}
}  // namespace a2a::server::engine
