// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/request_context.h"

namespace a2a::server {

class HttpByteTransport;

struct RestEndpointPaths final {
  static constexpr std::string_view kSendMessage = "/message:send";
  static constexpr std::string_view kSendStreamingMessage = "/message:stream";
  static constexpr std::string_view kTaskCollection = "/tasks";
  static constexpr std::string_view kTaskResourcePrefix = "/tasks/";
  static constexpr std::string_view kTaskCancelSuffix = ":cancel";
  static constexpr std::string_view kTaskSubscribeSuffix = ":subscribe";
  static constexpr std::string_view kTaskSubscribePath = "/tasks/{id}:subscribe";
};

struct RestRequest final {
  std::string method;
  std::string path;
  std::unordered_map<std::string, std::string> query_params;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  RequestContext context;
};

struct RestResponse final {
  static constexpr int kDefaultHttpStatus = 500;
  int http_status = kDefaultHttpStatus;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  std::function<core::Result<void>(HttpByteTransport&)> stream_writer;
};

struct RestRoute final {
  std::string_view method;
  std::string_view path_pattern;
  DispatcherOperation operation;
};

class RestTransport final {
 public:
  explicit RestTransport(Dispatcher* dispatcher);

  [[nodiscard]] static const std::vector<RestRoute>& Routes();
  [[nodiscard]] core::Result<RestResponse> Handle(const RestRequest& request) const;

 private:
  [[nodiscard]] static std::optional<DispatchRequest> BuildDispatchRequest(const RestRequest& request);
  [[nodiscard]] static core::Result<RestResponse> SerializeDispatchResponse(DispatcherOperation operation,
                                                                            DispatchResponse& response);
  [[nodiscard]] static RestResponse BuildErrorResponse(const core::Error& error);

  Dispatcher* dispatcher_ = nullptr;
};

}  // namespace a2a::server
