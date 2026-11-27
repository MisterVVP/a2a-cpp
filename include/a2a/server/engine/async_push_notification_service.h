// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/awaitable.hpp>
#include <memory>

#include "a2a/server/async_push_notification_delivery.h"
#include "a2a/server/engine/request_execution.h"
#include "a2a/server/push_notification_store.h"

namespace a2a::server::engine {
class AsyncPushNotificationService final {
 public:
  static boost::asio::awaitable<core::Result<PushDeliveryResult>> Deliver(
      std::shared_ptr<AsyncPushNotificationDeliveryClient> client, PushDeliveryRequest request,
      RequestExecutionContext context, AsyncPushDeliveryOptions options = {});
  static boost::asio::awaitable<core::Result<void>> NotifyTaskUpdated(
      PushNotificationStore& store, std::shared_ptr<AsyncPushNotificationDeliveryClient> client, lf::a2a::v1::Task task,
      RequestExecutionContext context, AsyncPushDeliveryOptions options = {});
};
}  // namespace a2a::server::engine
