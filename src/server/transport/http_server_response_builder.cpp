// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_server_response_builder.h"

#include <string>
#include <utility>

#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/version.h"
#include "a2a/server/rest_transport.h"

namespace a2a::server {

HttpServerResponseBuilder HttpServerResponseBuilder::FromRestResponse(const RestResponse& response) {
  HttpServerResponseBuilder builder;
  builder.response_.status_code = response.http_status;
  builder.response_.headers = response.headers;
  builder.response_.body = response.body;
  builder.response_.stream_writer = response.stream_writer;
  return builder;
}

HttpServerResponseBuilder HttpServerResponseBuilder::FromHttpResponse(HttpServerResponse response) {
  HttpServerResponseBuilder builder;
  builder.response_ = std::move(response);
  return builder;
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithStatus(int status_code) {
  response_.status_code = status_code;
  return *this;
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithBody(std::string body) {
  response_.body = std::move(body);
  return *this;
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithStreamWriter(
    std::function<core::Result<void>(HttpByteTransport&)> stream_writer) {
  response_.stream_writer = std::move(stream_writer);
  return *this;
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithHeader(std::string_view name, std::string value) {
  response_.headers.insert_or_assign(std::string(name), std::move(value));
  return *this;
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithJsonContentType() {
  return WithHeader(core::http::kContentTypeHeaderName, std::string(core::http::kContentTypeApplicationJson));
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithSseContentType() {
  return WithHeader(core::http::kContentTypeHeaderName, std::string(core::http::kContentTypeTextEventStream));
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithCacheControlNoCache() {
  return WithHeader(core::http::kCacheControlHeaderName, std::string(core::http::kCacheControlNoCache));
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithA2aVersion() {
  return WithHeader(core::Version::kHeaderName, core::Version::HeaderValue());
}

HttpServerResponseBuilder& HttpServerResponseBuilder::WithActivatedExtensions(
    const std::vector<std::string>& activated_extensions) {
  if (!activated_extensions.empty()) {
    WithHeader(core::Extensions::kHeaderName, core::Extensions::Format(activated_extensions));
  }
  return *this;
}

HttpServerResponse HttpServerResponseBuilder::Build() { return std::move(response_); }

}  // namespace a2a::server
