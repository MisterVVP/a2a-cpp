// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/rest_server_transport.h"

namespace a2a::server {

class HttpByteTransport;
struct RestResponse;

class HttpServerResponseBuilder final {
 public:
  [[nodiscard]] static HttpServerResponseBuilder FromRestResponse(const RestResponse& response);
  [[nodiscard]] static HttpServerResponseBuilder FromHttpResponse(HttpServerResponse response);

  HttpServerResponseBuilder& WithStatus(int status_code);
  HttpServerResponseBuilder& WithBody(std::string body);
  HttpServerResponseBuilder& WithStreamWriter(std::function<core::Result<void>(HttpByteTransport&)> stream_writer);
  HttpServerResponseBuilder& WithHeader(std::string_view name, std::string value);
  HttpServerResponseBuilder& WithJsonContentType();
  HttpServerResponseBuilder& WithSseContentType();
  HttpServerResponseBuilder& WithCacheControlNoCache();
  HttpServerResponseBuilder& WithA2aVersion();
  HttpServerResponseBuilder& WithActivatedExtensions(const std::vector<std::string>& activated_extensions);

  [[nodiscard]] HttpServerResponse Build();

 private:
  HttpServerResponse response_;
};

}  // namespace a2a::server
