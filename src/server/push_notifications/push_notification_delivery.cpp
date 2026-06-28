// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_delivery.h"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/protojson.h"

namespace a2a::server {
namespace {

constexpr std::string_view kPostMethod = "POST";
constexpr std::string_view kUnsupportedSchemeMessage = "push notification URL must use http or https";
constexpr std::string_view kUnsupportedHttpVersionMessage =
    "push notification delivery supports only HTTP/1.1, HTTP/2.0, or HTTP/3.0";
constexpr std::string_view kWebhookNonSuccessMessage = "webhook returned non-2xx status";

core::Result<void> ValidateUrlScheme(std::string_view url) {
  if (url.starts_with(core::http::kHttpScheme) || url.starts_with(core::http::kHttpsScheme)) {
    return {};
  }
  return core::Error::Validation(std::string(kUnsupportedSchemeMessage));
}

core::Result<void> ValidateHttpVersion(std::string_view http_version) {
  if (http::IsSupportedHttpVersion(http_version)) {
    return {};
  }
  return core::Error::Validation(std::string(kUnsupportedHttpVersionMessage));
}

core::Result<void> ValidateDeliveryOptions(const HttpPushNotificationDeliveryOptions& options) {
  const auto primary = ValidateHttpVersion(options.http_version);
  if (!primary.ok()) {
    return primary.error();
  }
  if (options.fallback_http_version.empty()) {
    return {};
  }
  return ValidateHttpVersion(options.fallback_http_version);
}

std::string BuildAuthorizationValue(const lf::a2a::v1::TaskPushNotificationConfig& config) {
  std::string header;
  header.reserve(config.authentication().scheme().size() + config.authentication().credentials().size() + 1);
  header.append(config.authentication().scheme());
  if (!config.authentication().credentials().empty()) {
    header.push_back(' ');
    header.append(config.authentication().credentials());
  }
  return header;
}

std::vector<http::Header> BuildHeaders(const lf::a2a::v1::TaskPushNotificationConfig& config) {
  std::vector<http::Header> headers;
  constexpr std::size_t kBaseHeaderCount = 1;
  headers.reserve(kBaseHeaderCount + (config.authentication().scheme().empty() ? 0U : 1U));
  headers.push_back(http::Header{.name = std::string(core::http::kContentTypeHeaderName),
                                 .value = std::string(core::http::kContentTypeApplicationJson)});
  if (!config.authentication().scheme().empty()) {
    headers.push_back(http::Header{.name = std::string(core::http::kAuthorizationHeaderName),
                                   .value = BuildAuthorizationValue(config)});
  }
  return headers;
}

http::Request BuildHttpRequest(const PushDeliveryRequest& request, std::string body, std::string_view http_version,
                               std::chrono::milliseconds timeout) {
  http::Request http_request;
  http_request.method = std::string(kPostMethod);
  http_request.url = request.config.url();
  http_request.headers = BuildHeaders(request.config);
  http_request.body = std::move(body);
  http_request.timeout = timeout;
  http_request.http_version = std::string(http_version);
  return http_request;
}

core::Result<PushDeliveryResult> DeliverHttpRequest(const http::Client& http_client, const PushDeliveryRequest& request,
                                                    const std::string& body, std::string_view http_version,
                                                    std::chrono::milliseconds timeout) {
  const auto response = http_client.SendRequest(BuildHttpRequest(request, body, http_version, timeout));
  if (!response.ok()) {
    return response.error();
  }

  const auto http_status = response.value().status_code;
  if (http_status < core::http::kSuccessStatusMin || http_status > core::http::kSuccessStatusMax) {
    return core::Error::RemoteProtocol(std::string(kWebhookNonSuccessMessage)).WithHttpStatus(http_status);
  }
  return PushDeliveryResult{.http_status = http_status, .error_message = {}};
}

}  // namespace

HttpPushNotificationDeliveryClient::HttpPushNotificationDeliveryClient(HttpPushNotificationDeliveryOptions options)
    : options_(std::move(options)) {}

HttpPushNotificationDeliveryClient::HttpPushNotificationDeliveryClient(std::chrono::milliseconds timeout)
    : options_(HttpPushNotificationDeliveryOptions{.timeout = timeout}) {}

core::Result<PushDeliveryResult> HttpPushNotificationDeliveryClient::Deliver(const PushDeliveryRequest& request) {
  const auto valid_url = ValidateUrlScheme(request.config.url());
  if (!valid_url.ok()) {
    return valid_url.error();
  }

  const auto valid_options = ValidateDeliveryOptions(options_);
  if (!valid_options.ok()) {
    return valid_options.error();
  }

  const auto body = core::MessageToJson(request.payload);
  if (!body.ok()) {
    return body.error();
  }

  auto result = DeliverHttpRequest(http_client_, request, body.value(), options_.http_version, options_.timeout);
  if (result.ok() || options_.fallback_http_version.empty() ||
      options_.fallback_http_version == options_.http_version) {
    return result;
  }
  return DeliverHttpRequest(http_client_, request, body.value(), options_.fallback_http_version, options_.timeout);
}

}  // namespace a2a::server
