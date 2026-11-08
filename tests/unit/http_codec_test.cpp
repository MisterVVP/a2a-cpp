// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include <gtest/gtest.h>

#include <string>

#include "a2a/core/http_constants.h"
#include "a2a/server/http_adapter.h"

namespace a2a::server {
namespace {
constexpr std::string_view kRequest = "POST /rpc HTTP/1.1\r\nContent-Length: 4\r\n\r\ndata";
constexpr std::string_view kHeaderless = "GET / HTTP/1.1\r\n\r\n";
constexpr std::string_view kConflicting = "POST / HTTP/1.1\r\nContent-Length: 1\r\ncontent-length: 2\r\n\r\na";
constexpr std::string_view kChunked = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
constexpr std::string_view kOverflow = "POST / HTTP/1.1\r\nContent-Length: 18446744073709551616\r\n\r\n";
constexpr std::size_t kSmallLimit = 20;

void ExpectIncomplete(std::string_view bytes) {
  HttpAdapter codec;
  std::size_t consumed = 1;
  auto result = codec.ParseRequest(bytes, consumed);
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().has_value());
  EXPECT_EQ(consumed, 0U);
}

void ExpectFirstRequest(std::string& bytes) {
  HttpAdapter codec;
  std::size_t consumed = 0;
  const auto result = codec.ParseRequest(bytes, consumed);
  ASSERT_TRUE(result.ok());
  const auto& first = result.value();
  if (!first.has_value()) {
    ADD_FAILURE();
    return;
  }
  EXPECT_EQ(consumed, kRequest.size());
  EXPECT_EQ(first->body, kRequest.substr(kRequest.size() - 4U));
  bytes.erase(0, consumed);
}

TEST(HttpCodec, EveryFragmentBoundary) {
  for (std::size_t size = 0; size < kRequest.size(); ++size) {
    ExpectIncomplete(kRequest.substr(0, size));
  }
}

TEST(HttpCodec, PipeliningBoundaryAndHeaderlessRequest) {
  std::string bytes(kRequest);
  bytes.append(kHeaderless);
  ExpectFirstRequest(bytes);
  HttpAdapter codec;
  std::size_t consumed = 0;
  const auto second = codec.ParseRequest(bytes, consumed);
  ASSERT_TRUE(second.ok());
  EXPECT_TRUE(second.value().has_value());
  EXPECT_EQ(consumed, kHeaderless.size());
}

TEST(HttpCodec, MalformedFramingAndLimits) {
  const HttpAdapter codec;
  for (const auto bytes : {kConflicting, kChunked, kOverflow}) {
    std::size_t consumed = 1;
    EXPECT_FALSE(codec.ParseRequest(bytes, consumed).ok());
    EXPECT_EQ(consumed, 0U);
  }
  const HttpAdapter small({.max_request_size = kSmallLimit});
  std::size_t consumed = 0;
  EXPECT_FALSE(small.ParseRequest(kRequest, consumed).ok());
}

TEST(HttpCodec, ResponseLengthValidation) {
  HttpServerResponse response;
  response.status_code = core::http::kStatusOk;
  response.body = kRequest;
  response.headers.emplace("Content-Length", "1");
  EXPECT_FALSE(HttpAdapter::EncodeResponse(response).ok());
}
TEST(HttpCodec, ChunkedClosingHeadKeepsConnectionSemantics) {
  constexpr std::string_view kChunkedHeader = "Transfer-Encoding: chunked";
  constexpr std::string_view kCloseHeader = "Connection: close";
  HttpServerResponse response;
  response.status_code = core::http::kStatusOk;
  const auto bytes = HttpAdapter::EncodeResponseHead(response, true, true);
  ASSERT_TRUE(bytes.ok());
  EXPECT_NE(bytes.value().find(kChunkedHeader), std::string::npos);
  EXPECT_NE(bytes.value().find(kCloseHeader), std::string::npos);
}

void ExpectPending(const HttpAdapter& codec, HttpConnectionState& state, std::string_view bytes) {
  std::size_t consumed = 1;
  auto result = codec.ParseRequest(bytes, state, consumed);
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(result.value().has_value());
  EXPECT_EQ(consumed, 0U);
}
void ExpectDecoded(const HttpAdapter& codec, HttpConnectionState& state, std::string_view bytes,
                   std::string_view body) {
  std::size_t consumed = 0;
  auto result = codec.ParseRequest(bytes, state, consumed);
  ASSERT_TRUE(result.ok());
  const auto& request = result.value();
  if (!request.has_value()) {
    ADD_FAILURE();
    return;
  }
  EXPECT_EQ(request->body, body);
  EXPECT_EQ(consumed, bytes.size());
}

TEST(HttpCodec, StatefulFragmentsCacheHeadersAndResetAfterCompletion) {
  HttpAdapter codec;
  HttpConnectionState state;
  for (std::size_t size = 0; size < kRequest.size(); ++size) {
    ExpectPending(codec, state, kRequest.substr(0, size));
  }
  constexpr std::string_view kBody = "data";
  ExpectDecoded(codec, state, kRequest, kBody);
  ExpectDecoded(codec, state, kHeaderless, {});
}

TEST(HttpCodec, StatefulDecoderRejectsShrinkingIncompleteInput) {
  HttpAdapter codec;
  HttpConnectionState state;
  ExpectPending(codec, state, kRequest.substr(0, kRequest.size() - 1));
  std::size_t consumed = 1;
  EXPECT_FALSE(codec.ParseRequest({}, state, consumed).ok());
  EXPECT_EQ(consumed, 0U);
}

}  // namespace
}  // namespace a2a::server
