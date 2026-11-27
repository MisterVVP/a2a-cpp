// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "a2a/server/push_notification_delivery.h"

namespace a2a::server::delivery_internal {
core::Result<void> ValidateUrlScheme(std::string_view url);
core::Result<void> ValidateDeliveryOptions(const HttpPushNotificationDeliveryOptions& options);
http::Request BuildHttpRequest(const PushDeliveryRequest& request, std::string body, std::string_view http_version,
                               std::chrono::milliseconds timeout);
core::Result<PushDeliveryResult> MapResponse(const http::Response& response);
}  // namespace a2a::server::delivery_internal
