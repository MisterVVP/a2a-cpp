// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <string>

#include "a2a/core/http_constants.h"
#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

struct PushDeliveryRequest final {
  lf::a2a::v1::TaskPushNotificationConfig config;
  lf::a2a::v1::StreamResponse payload;
};

struct PushDeliveryResult final {
  int http_status = 0;
  std::string error_message;
};

class PushNotificationDeliveryClient {
 public:
  virtual ~PushNotificationDeliveryClient() = default;

  [[nodiscard]] virtual core::Result<PushDeliveryResult> Deliver(const PushDeliveryRequest& request) = 0;
};

struct HttpPushNotificationDeliveryOptions final {
  std::chrono::milliseconds timeout = std::chrono::milliseconds(5000);
  std::string http_version = std::string(core::http::kDefaultPushHttpVersion);
  std::string fallback_http_version = std::string(core::http::kHttpVersion11);
};

class HttpPushNotificationDeliveryClient final : public PushNotificationDeliveryClient {
 public:
  explicit HttpPushNotificationDeliveryClient(HttpPushNotificationDeliveryOptions options = {});
  explicit HttpPushNotificationDeliveryClient(std::chrono::milliseconds timeout);

  [[nodiscard]] core::Result<PushDeliveryResult> Deliver(const PushDeliveryRequest& request) override;

 private:
  HttpPushNotificationDeliveryOptions options_;
};

}  // namespace a2a::server
