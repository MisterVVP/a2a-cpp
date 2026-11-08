// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/async_push_notification_delivery.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

#include "a2a/core/protojson.h"
#include "push_notification_delivery_internal.h"

namespace a2a::server {
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto kRequesterFailure = "push HTTP async requester threw during initiation";
constexpr auto kMissingRequester = "push HTTP async requester is not configured";

Clock::time_point DeliveryDeadline(std::chrono::milliseconds timeout, const AsyncPushDeliveryOptions& options) {
  const auto now = Clock::now();
  const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
  timeout = std::max(timeout, std::chrono::milliseconds::zero());
  const auto deadline = timeout >= maximum ? Clock::time_point::max() : now + timeout;
  return std::max(now, options.deadline ? std::min(deadline, *options.deadline) : deadline);
}

core::Result<core::AsyncHandle> StartRequest(const PushHttpAsyncRequester& requester, http::Request request,
                                             core::Completion<http::Response> completion) {
  try {
    return requester(std::move(request), std::move(completion));
  } catch (...) {
    return core::Error::Internal(kRequesterFailure);
  }
}

struct Attempt final {
  void Cancel() {
    std::optional<core::AsyncHandle> current;
    {
      std::lock_guard lock(mutex);
      cancelled = true;
      current = handle;
    }
    if (current) {
      current->Cancel();
    }
  }
  void Attach(const core::AsyncHandle& value) {
    bool cancel = false;
    {
      std::lock_guard lock(mutex);
      handle = value;
      cancel = cancelled;
    }
    if (cancel) {
      value.Cancel();
    }
  }
  std::atomic<bool> completion_claimed{false};
  std::mutex mutex;
  std::optional<core::AsyncHandle> handle;
  bool cancelled = false;
};

struct Delivery final : std::enable_shared_from_this<Delivery> {
  Delivery(PushDeliveryRequest input, HttpPushNotificationDeliveryOptions settings, PushHttpAsyncRequester send,
           core::AsyncCompletion<PushDeliveryResult> output, Clock::time_point expires, std::string payload)
      : request(std::move(input)),
        options(std::move(settings)),
        requester(std::move(send)),
        completion(std::move(output)),
        deadline(expires),
        body(std::move(payload)) {}

  void Start(bool fallback = false) {
    if (!completion.IsActive()) {
      return;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      completion.Complete(core::Error::Network(kDeadline).WithProtocolCode(core::AsyncHandle::kDeadlineExceeded));
      return;
    }
    auto http_request = delivery_internal::BuildHttpRequest(
        request, body, fallback ? options.fallback_http_version : options.http_version, remaining);
    auto attempt = std::make_shared<Attempt>();
    completion.RegisterCancellation([attempt] { attempt->Cancel(); });
    auto started =
        StartRequest(requester, std::move(http_request), [self = shared_from_this(), fallback, attempt](auto response) {
          if (!attempt->completion_claimed.exchange(true)) {
            self->Finish(std::move(response), fallback);
          }
        });
    if (!started.ok()) {
      if (!attempt->completion_claimed.exchange(true)) {
        Finish(started.error(), fallback);
      }
      return;
    }
    attempt->Attach(started.value());
  }

  void Finish(core::Result<http::Response> response, bool fallback) {
    auto result = response.ok() ? delivery_internal::MapResponse(response.value())
                                : core::Result<PushDeliveryResult>(response.error());
    if (!result.ok() && !fallback && !options.fallback_http_version.empty() &&
        options.fallback_http_version != options.http_version &&
        result.error().protocol_code() != core::AsyncHandle::kCancelled &&
        result.error().protocol_code() != core::AsyncHandle::kDeadlineExceeded) {
      Start(true);
      return;
    }
    completion.Complete(std::move(result));
  }
  static constexpr auto kDeadline = "push delivery deadline exceeded";
  PushDeliveryRequest request;
  HttpPushNotificationDeliveryOptions options;
  PushHttpAsyncRequester requester;
  core::AsyncCompletion<PushDeliveryResult> completion;
  Clock::time_point deadline;
  std::string body;
};
}  // namespace

struct HttpAsyncPushNotificationDeliveryClient::State final {
  HttpPushNotificationDeliveryOptions options;
  PushHttpAsyncRequester requester;
  std::shared_ptr<http::Client> native;
  core::AsyncScope scope;
};

HttpAsyncPushNotificationDeliveryClient::HttpAsyncPushNotificationDeliveryClient(
    HttpPushNotificationDeliveryOptions options)
    : HttpAsyncPushNotificationDeliveryClient(std::move(options), {}) {
  state_->native = std::make_shared<http::Client>();
  state_->requester = [client = state_->native](http::Request request, core::Completion<http::Response> completion) {
    return client->SendRequestAsync(std::move(request), std::move(completion));
  };
}
HttpAsyncPushNotificationDeliveryClient::HttpAsyncPushNotificationDeliveryClient(
    HttpPushNotificationDeliveryOptions options, PushHttpAsyncRequester requester)
    : state_(std::make_shared<State>()) {
  state_->options = std::move(options);
  state_->requester = std::move(requester);
}
HttpAsyncPushNotificationDeliveryClient::~HttpAsyncPushNotificationDeliveryClient() { Shutdown(); }
void HttpAsyncPushNotificationDeliveryClient::Shutdown() {
  state_->scope.Shutdown();
  if (state_->native) {
    state_->native->Shutdown();
  }
}
core::Result<core::AsyncHandle> HttpAsyncPushNotificationDeliveryClient::DeliverAsync(
    const PushDeliveryRequest& request, core::Completion<PushDeliveryResult> completion,
    const AsyncPushDeliveryOptions& options) {
  const auto state = state_;
  const auto deadline = DeliveryDeadline(options.timeout.value_or(state->options.timeout), options);
  auto started = state->scope.Start<PushDeliveryResult>(
      std::move(completion), std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now()));
  if (!started.ok()) {
    return started.error();
  }
  auto token = std::move(started.value());
  const auto url = delivery_internal::ValidateUrlScheme(request.config.url());
  const auto settings = delivery_internal::ValidateDeliveryOptions(state->options);
  if (!url.ok()) {
    token.Complete(url.error());
    return token.handle();
  }
  if (!settings.ok()) {
    token.Complete(settings.error());
    return token.handle();
  }
  if (!state->requester) {
    token.Complete(core::Error::Validation(kMissingRequester));
    return token.handle();
  }
  auto body = core::MessageToJson(request.payload);
  if (!body.ok()) {
    token.Complete(body.error());
    return token.handle();
  }
  auto delivery =
      std::make_shared<Delivery>(request, state->options, state->requester, token, deadline, std::move(body.value()));
  delivery->Start();
  return token.handle();
}
}  // namespace a2a::server
