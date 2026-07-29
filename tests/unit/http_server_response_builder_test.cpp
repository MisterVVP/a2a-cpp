// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_server_response_builder.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/version.h"
#include "a2a/server/rest_transport.h"

namespace {

constexpr int kExpectedStatus = a2a::core::http::kStatusAccepted;
constexpr std::string_view kResponseBody = R"({"result":"ok"})";
constexpr std::string_view kExistingHeaderName = "X-Existing";
constexpr std::string_view kExistingHeaderValue = "preserved";
constexpr std::string_view kFirstExtension = "https://example.com/extensions/first";
constexpr std::string_view kSecondExtension = "https://example.com/extensions/second";

TEST(HttpServerResponseBuilderTest, BuildsJsonResponseWithCommonHeaders) {
  const auto response = a2a::server::HttpServerResponseBuilder()
                            .WithStatus(kExpectedStatus)
                            .WithJsonContentType()
                            .WithA2aVersion()
                            .WithBody(std::string(kResponseBody))
                            .Build();

  EXPECT_EQ(response.status_code, kExpectedStatus);
  EXPECT_EQ(response.body, kResponseBody);
  EXPECT_EQ(response.headers.at(std::string(a2a::core::http::kContentTypeHeaderName)),
            a2a::core::http::kContentTypeApplicationJson);
  EXPECT_EQ(response.headers.at(std::string(a2a::core::Version::kHeaderName)), a2a::core::Version::HeaderValue());
}

TEST(HttpServerResponseBuilderTest, BuildsSseResponseWithCommonHeaders) {
  const auto response = a2a::server::HttpServerResponseBuilder()
                            .WithStatus(a2a::core::http::kStatusOk)
                            .WithSseContentType()
                            .WithCacheControlNoCache()
                            .WithA2aVersion()
                            .Build();

  EXPECT_EQ(response.headers.at(std::string(a2a::core::http::kContentTypeHeaderName)),
            a2a::core::http::kContentTypeTextEventStream);
  EXPECT_EQ(response.headers.at(std::string(a2a::core::http::kCacheControlHeaderName)),
            a2a::core::http::kCacheControlNoCache);
  EXPECT_EQ(response.headers.at(std::string(a2a::core::Version::kHeaderName)), a2a::core::Version::HeaderValue());
}

TEST(HttpServerResponseBuilderTest, OmitsEmptyActivatedExtensions) {
  const auto response =
      a2a::server::HttpServerResponseBuilder().WithActivatedExtensions(std::vector<std::string>{}).Build();

  EXPECT_FALSE(response.headers.contains(std::string(a2a::core::Extensions::kHeaderName)));
}

TEST(HttpServerResponseBuilderTest, FormatsActivatedExtensions) {
  const std::vector<std::string> extensions{std::string(kFirstExtension), std::string(kSecondExtension)};
  const auto response = a2a::server::HttpServerResponseBuilder().WithActivatedExtensions(extensions).Build();

  EXPECT_EQ(response.headers.at(std::string(a2a::core::Extensions::kHeaderName)),
            a2a::core::Extensions::Format(extensions));
}

TEST(HttpServerResponseBuilderTest, AdaptsRestResponseWithoutLosingData) {
  a2a::server::RestResponse rest_response;
  rest_response.http_status = kExpectedStatus;
  rest_response.headers.emplace(kExistingHeaderName, kExistingHeaderValue);
  rest_response.body = kResponseBody;
  rest_response.stream_writer = [](a2a::server::HttpByteTransport&) -> a2a::core::Result<void> { return {}; };

  const auto response = a2a::server::HttpServerResponseBuilder::FromRestResponse(rest_response).Build();

  EXPECT_EQ(response.status_code, rest_response.http_status);
  EXPECT_EQ(response.headers.at(std::string(kExistingHeaderName)), kExistingHeaderValue);
  EXPECT_EQ(response.body, rest_response.body);
  EXPECT_TRUE(static_cast<bool>(response.stream_writer));
}

}  // namespace
