// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

#include "a2a/core/error.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/protocol_methods.h"
#include "a2a/http/http_client.h"

namespace a2a::client::http_json_internal {

struct EndpointMap final {
  static constexpr std::string_view kSendMessage = "/message:send";
  static constexpr std::string_view kSendStreamingMessage = "/message:stream";
  static constexpr std::string_view kTaskCollection = "/tasks";
  static constexpr std::string_view kPushConfigCollection = core::protocol_methods::kPushNotificationConfigsSegment;
};

[[nodiscard]] a2a::http::Request ToSharedHttpRequest(const HttpRequest& request);
[[nodiscard]] HttpClientResponse ToClientHttpResponse(a2a::http::Response response);

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
[[nodiscard]] std::string JoinUrl(std::string_view interface_base_url, std::string_view rpc_endpoint);
[[nodiscard]] core::Result<void> ValidateResponseVersion(const HttpClientResponse& response);
[[nodiscard]] core::Error BuildHttpError(std::string_view method, std::string_view endpoint,
                                         const HttpClientResponse& response);
[[nodiscard]] std::string BuildTaskPath(std::string_view task_id);

}  // namespace a2a::client::http_json_internal
