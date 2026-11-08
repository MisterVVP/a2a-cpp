// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "a2a/server/async_push_notification_delivery.h"

namespace a2a::server::testing {
inline constexpr auto kPushWait = std::chrono::seconds(5);
inline constexpr auto kPushLong = std::chrono::seconds(30);
inline constexpr auto kPushShort = std::chrono::milliseconds(30);
inline constexpr auto kPushTask = "push-task";
inline constexpr auto kPushConfig = "push-config";
inline constexpr auto kPushUrl = "http://127.0.0.1:1/webhook";
inline constexpr auto kPushBearer = "Bearer";
inline constexpr auto kPushCredential = "test-credential";
inline constexpr auto kPushUnsupportedUrl = "ftp://127.0.0.1/webhook";
inline constexpr auto kPushFailure = "fake network failure";
inline constexpr auto kPushAuthorization = "Bearer test-credential";
inline constexpr auto kPushUnknownVersion = "HTTP/0.9";
inline constexpr int kPushSuccess = 204;
inline constexpr int kPushError = 503;
inline constexpr std::size_t kPushFirst = 0;
inline constexpr std::size_t kPushSecond = 1;
inline constexpr std::size_t kPushAttempts = 2;

inline HttpPushNotificationDeliveryOptions PushOptions() {
  return {.timeout = kPushLong, .http_version = std::string(core::http::kHttpVersion11), .fallback_http_version = {}};
}
inline PushDeliveryRequest PushRequest(std::string url = kPushUrl) {
  PushDeliveryRequest request;
  request.config.set_task_id(kPushTask);
  request.config.set_id(kPushConfig);
  request.config.set_url(std::move(url));
  request.config.mutable_authentication()->set_scheme(kPushBearer);
  request.config.mutable_authentication()->set_credentials(kPushCredential);
  auto* update = request.payload.mutable_status_update();
  update->set_task_id(kPushTask);
  update->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  return request;
}

struct PushProbe final {
  core::Completion<PushDeliveryResult> Callback() {
    return [this](auto value) {
      if (++calls == 1U) {
        promise.set_value(std::move(value));
      }
    };
  }
  core::Result<PushDeliveryResult> Get() {
    const auto ready = future.wait_for(kPushWait);
    EXPECT_EQ(ready, std::future_status::ready);
    if (ready != std::future_status::ready) {
      return core::Error::Internal(kPushFailure);
    }
    return future.get();
  }
  std::atomic<std::size_t> calls{0};
  std::promise<core::Result<PushDeliveryResult>> promise;
  std::future<core::Result<PushDeliveryResult>> future = promise.get_future();
};

// Uses only the shared completion runtime. Tests explicitly release transfers;
// cancellation and timeout are observable on the returned native handle.
class FakePushRequester final {
 public:
  PushHttpAsyncRequester Requester() {
    return [this](http::Request request, core::Completion<http::Response> completion) {
      auto started = scope_.Start<http::Response>(std::move(completion), request.timeout);
      if (!started.ok()) {
        return core::Result<core::AsyncHandle>(started.error());
      }
      auto token = std::move(started.value());
      {
        std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
        tokens_.push_back(token);
      }
      changed_.notify_all();
      return core::Result<core::AsyncHandle>(token.handle());
    };
  }
  bool WaitFor(std::size_t count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kPushWait, [&] { return requests_.size() >= count; });
  }
  std::size_t Count() {
    std::lock_guard lock(mutex_);
    return requests_.size();
  }
  http::Request Request(std::size_t index = kPushFirst) {
    std::lock_guard lock(mutex_);
    return requests_.at(index);
  }
  core::AsyncCompletion<http::Response> Token(std::size_t index = kPushFirst) {
    std::lock_guard lock(mutex_);
    return tokens_.at(index);
  }
  void Complete(int status = kPushSuccess, std::size_t index = kPushFirst) {
    Token(index).Complete(http::Response{.status_code = status, .headers = {}, .body = {}});
  }
  ~FakePushRequester() { scope_.Shutdown(); }

 private:
  core::AsyncScope scope_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<http::Request> requests_;
  std::vector<core::AsyncCompletion<http::Response>> tokens_;
};
}  // namespace a2a::server::testing
