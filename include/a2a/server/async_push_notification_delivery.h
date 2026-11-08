// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>

#include "a2a/core/async_operation.h"
#include "a2a/core/non_copyable.h"
#include "a2a/server/push_notification_delivery.h"

namespace a2a::server {
struct AsyncPushDeliveryOptions final {
  // Absent uses the client's default; zero/negative expires immediately.
  std::optional<std::chrono::milliseconds> timeout;
  std::optional<std::chrono::steady_clock::time_point> deadline;
};

// Parallel contract: existing synchronous clients/executors remain unchanged.
class AsyncPushNotificationDeliveryClient : private core::NonCopyableOrMovable {
 public:
  virtual ~AsyncPushNotificationDeliveryClient() = default;
  // Copies the request before returning. Rejection returns an error without a
  // callback. Acceptance returns a handle and exactly one completion, including
  // cancellation/deadline/shutdown, on shared SDK workers, never inline/reactor.
  [[nodiscard]] virtual core::Result<core::AsyncHandle> DeliverAsync(const PushDeliveryRequest& request,
                                                                     core::Completion<PushDeliveryResult> completion,
                                                                     const AsyncPushDeliveryOptions& options = {}) = 0;
  // Closes admission, cancels requests and drains callbacks. From an SDK callback
  // cancellation is reentrant and draining is deferred. Safe to call repeatedly.
  virtual void Shutdown() = 0;
};

// Requester follows http::Client::SendRequestAsync's ownership/completion contract.
// Injected requesters must support concurrent calls and prompt cancellation;
// their resources must outlive their accepted requests. No blocking adapters.
using PushHttpAsyncRequester =
    std::function<core::Result<core::AsyncHandle>(http::Request, core::Completion<http::Response>)>;

class HttpAsyncPushNotificationDeliveryClient final : public AsyncPushNotificationDeliveryClient {
 public:
  explicit HttpAsyncPushNotificationDeliveryClient(HttpPushNotificationDeliveryOptions options = {});
  HttpAsyncPushNotificationDeliveryClient(HttpPushNotificationDeliveryOptions options,
                                          PushHttpAsyncRequester requester);
  ~HttpAsyncPushNotificationDeliveryClient() override;
  [[nodiscard]] core::Result<core::AsyncHandle> DeliverAsync(const PushDeliveryRequest& request,
                                                             core::Completion<PushDeliveryResult> completion,
                                                             const AsyncPushDeliveryOptions& options = {}) override;
  void Shutdown() override;

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace a2a::server
