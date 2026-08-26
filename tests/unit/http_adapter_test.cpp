// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_adapter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/http_constants.h"

namespace {

class BufferTransport final : public a2a::server::HttpByteTransport {
 public:
  explicit BufferTransport(std::string input) : input_(std::move(input)) {}

  a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    if (read_offset_ >= input_.size()) {
      return static_cast<std::size_t>(0);
    }
    const std::size_t to_copy = std::min(size, input_.size() - read_offset_);
    std::copy_n(input_.data() + read_offset_, to_copy, buffer);
    read_offset_ += to_copy;
    return to_copy;
  }

  a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    if (write_zero_bytes_) {
      return static_cast<std::size_t>(0);
    }
    const std::size_t bytes_to_write = partial_write_limit_ == 0 ? size : std::min(size, partial_write_limit_);
    output_.append(buffer, bytes_to_write);
    return bytes_to_write;
  }

  void set_partial_write_limit(std::size_t limit) noexcept { partial_write_limit_ = limit; }
  void set_write_zero_bytes(bool enabled) noexcept { write_zero_bytes_ = enabled; }

  [[nodiscard]] const std::string& output() const { return output_; }

 private:
  std::string input_;
  std::size_t read_offset_ = 0;
  std::size_t partial_write_limit_ = 0;
  bool write_zero_bytes_ = false;
  std::string output_;
};

constexpr std::string_view kBody = "hello";
constexpr std::string_view kJsonBody = "{}";
constexpr std::string_view kSseChunk = "data: {}\n\n";
constexpr std::string_view kPostMethod = "POST";
constexpr std::string_view kRpcPath = "/rpc";
constexpr std::string_view kHostHeaderName = "Host";
constexpr std::string_view kLocalhost = "localhost";
constexpr std::string_view kLowerContentLengthHeaderName = "content-length";
constexpr std::string_view kContentLengthFive = "5";
constexpr std::string_view kContentLengthTwo = "2";
constexpr std::string_view kContentLengthTooLarge = "999999999999999999999999";
constexpr std::string_view kInvalidContentLength = "not-a-number";
constexpr std::string_view kMismatchedContentLength = "99";
constexpr std::string_view kTestHeaderName = "X-Test";
constexpr std::string_view kTrueHeaderValue = "true";
constexpr std::string_view kConnectionHeaderValueWithClose = "keep-alive, close";
constexpr std::string_view kConnectionCloseHeaderLine = "Connection: close";
constexpr std::string_view kTransferEncodingChunkedHeaderLine = "Transfer-Encoding: chunked\r\n";
constexpr std::string_view kFinalChunk = "0\r\n\r\n";
constexpr std::string_view kSseChunkSizeHex = "a";
constexpr std::string_view kJsonContentType = "application/json";
constexpr std::string_view kStatusOkSuffix = " 200 OK";
constexpr std::string_view kHttp10Version = "HTTP/1.0";
constexpr std::string_view kAbsoluteRpcUrl = "http://localhost/rpc?debug=1";
constexpr std::string_view kAbsoluteUrlWithoutPath = "http://localhost";
constexpr std::string_view kDebugRpcTarget = "/rpc?debug=1";
constexpr std::string_view kEmptyContentLength = "   ";
constexpr std::string_view kDifferentContentLength = "4";
constexpr std::size_t kTinyReadBufferSize = 4U;
constexpr std::size_t kTinyMaxRequestSize = 8U;
constexpr std::size_t kPartialWriteLimit = 3;
constexpr bool kEnableZeroByteWrites = true;
constexpr int kHttpOk = 200;
constexpr int kHttpUnknown = 599;

std::string BuildRequest(std::string_view method, std::string_view target,
                         const std::vector<std::pair<std::string_view, std::string_view>>& headers,
                         std::string_view body = {}) {
  std::string request;
  request.append(method);
  request.push_back(' ');
  request.append(target);
  request.push_back(' ');
  request.append(a2a::core::http::kHttpVersion11);
  request.append(a2a::core::http::kLineTerminator);
  for (const auto& [name, value] : headers) {
    request.append(name);
    request.append(a2a::core::http::kHeaderNameValueSeparator);
    request.append(value);
    request.append(a2a::core::http::kLineTerminator);
  }
  request.append(a2a::core::http::kLineTerminator);
  request.append(body);
  return request;
}

std::string BuildExpectedStatusLine() {
  std::string line;
  line.append(a2a::core::http::kHttpVersion11);
  line.append(kStatusOkSuffix);
  line.append(a2a::core::http::kLineTerminator);
  return line;
}

std::string BuildExpectedContentLengthLine() {
  std::string line;
  line.append(a2a::core::http::kContentLengthHeaderName);
  line.append(a2a::core::http::kHeaderNameValueSeparator);
  line.append(kContentLengthTwo);
  line.append(a2a::core::http::kLineTerminator);
  return line;
}

std::string BuildRequestWithVersion(std::string_view method, std::string_view target, std::string_view version) {
  std::string request;
  request.append(method);
  request.push_back(' ');
  request.append(target);
  request.push_back(' ');
  request.append(version);
  request.append(a2a::core::http::kHeaderDelimiter);
  return request;
}

TEST(HttpAdapterTest, ParsesContentLengthCaseInsensitive) {
  BufferTransport transport(BuildRequest(kPostMethod, kRpcPath,
                                         {{kHostHeaderName, kLocalhost},
                                          {kLowerContentLengthHeaderName, kContentLengthFive},
                                          {kTestHeaderName, kTrueHeaderValue}},
                                         kBody));
  const a2a::server::HttpAdapter adapter;
  auto request = adapter.ReadRequest(transport, "127.0.0.1");
  ASSERT_TRUE(request.ok());
  EXPECT_EQ(request.value().method, "POST");
  EXPECT_EQ(request.value().target, "/rpc");
  EXPECT_EQ(request.value().body, kBody);
}

TEST(HttpAdapterTest, RetainsOverReadBytesForBackToBackBodyRequests) {
  std::string requests =
      BuildRequest(kPostMethod, kRpcPath, {{a2a::core::http::kContentLengthHeaderName, kContentLengthFive}}, kBody);
  requests.append(
      BuildRequest(kPostMethod, kRpcPath, {{a2a::core::http::kContentLengthHeaderName, kContentLengthTwo}}, kJsonBody));
  BufferTransport transport(std::move(requests));
  const a2a::server::HttpAdapter adapter;
  a2a::server::HttpConnectionState state;

  const auto first = adapter.ReadRequest(transport, state, "127.0.0.1");
  const auto second = adapter.ReadRequest(transport, state, "127.0.0.1");

  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(first.value().body, kBody);
  EXPECT_EQ(second.value().body, kJsonBody);
}

TEST(HttpAdapterTest, DetectsExplicitConnectionCloseTokenCaseInsensitively) {
  BufferTransport transport(
      BuildRequest(kPostMethod, kRpcPath, {{a2a::core::http::kConnectionHeaderName, kConnectionHeaderValueWithClose}}));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_TRUE(request.ok());
  EXPECT_FALSE(a2a::server::HttpAdapter::IsConnectionReusable(request.value()));
}

TEST(HttpAdapterTest, DuplicateConnectionHeadersPreserveCloseToken) {
  BufferTransport transport(
      BuildRequest(kPostMethod, kRpcPath,
                   {{a2a::core::http::kConnectionHeaderName, a2a::core::http::kConnectionCloseHeaderValue},
                    {a2a::core::http::kConnectionHeaderName, a2a::core::http::kConnectionKeepAliveHeaderValue}}));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_TRUE(request.ok());
  EXPECT_FALSE(a2a::server::HttpAdapter::IsConnectionReusable(request.value()));
}

TEST(HttpAdapterTest, RejectsTransferEncodingBeforePersistentReuse) {
  BufferTransport transport(
      BuildRequest(kPostMethod, kRpcPath,
                   {{a2a::core::http::kTransferEncodingHeaderName, a2a::core::http::kTransferEncodingChunked},
                    {a2a::core::http::kContentLengthHeaderName, kContentLengthFive}},
                   kBody));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, RejectsOverflowContentLength) {
  BufferTransport transport(BuildRequest(
      kPostMethod, kRpcPath,
      {{kHostHeaderName, kLocalhost}, {a2a::core::http::kContentLengthHeaderName, kContentLengthTooLarge}}));
  const a2a::server::HttpAdapter adapter;
  auto request = adapter.ReadRequest(transport, "127.0.0.1");
  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, NormalizesAbsoluteRequestTargetWithPath) {
  BufferTransport transport(BuildRequest(kPostMethod, kAbsoluteRpcUrl, {{kHostHeaderName, kLocalhost}}));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_TRUE(request.ok());
  EXPECT_EQ(request.value().target, kDebugRpcTarget);
}

TEST(HttpAdapterTest, NormalizesAbsoluteRequestTargetWithoutPathToRoot) {
  BufferTransport transport(BuildRequest(kPostMethod, kAbsoluteUrlWithoutPath, {{kHostHeaderName, kLocalhost}}));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_TRUE(request.ok());
  EXPECT_EQ(request.value().target, "/");
}

TEST(HttpAdapterTest, RejectsUnsupportedHttpVersion) {
  BufferTransport transport(BuildRequestWithVersion(kPostMethod, kRpcPath, kHttp10Version));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, RejectsMalformedHeaderLine) {
  BufferTransport transport(BuildRequest(kPostMethod, kRpcPath, {{"", kLocalhost}}));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, RejectsEmptyContentLength) {
  BufferTransport transport(
      BuildRequest(kPostMethod, kRpcPath,
                   {{kHostHeaderName, kLocalhost}, {a2a::core::http::kContentLengthHeaderName, kEmptyContentLength}}));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, RejectsConflictingContentLengthHeaders) {
  BufferTransport transport(BuildRequest(kPostMethod, kRpcPath,
                                         {{a2a::core::http::kContentLengthHeaderName, kContentLengthFive},
                                          {kLowerContentLengthHeaderName, kDifferentContentLength}},
                                         kBody));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, RejectsOversizedHeadersBeforeDelimiter) {
  BufferTransport transport(BuildRequest(kPostMethod, kRpcPath, {{kHostHeaderName, kLocalhost}}));
  const a2a::server::HttpAdapter adapter(
      {.max_request_size = kTinyMaxRequestSize, .read_buffer_size = kTinyReadBufferSize});

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, RejectsMissingBodyBytes) {
  BufferTransport transport(
      BuildRequest(kPostMethod, kRpcPath, {{a2a::core::http::kContentLengthHeaderName, kContentLengthFive}}));
  const a2a::server::HttpAdapter adapter;

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kInternal);
}

TEST(HttpAdapterTest, RejectsZeroReadBufferSize) {
  BufferTransport transport(BuildRequest(kPostMethod, kRpcPath, {{kHostHeaderName, kLocalhost}}));
  const a2a::server::HttpAdapter adapter({.read_buffer_size = 0U});

  const auto request = adapter.ReadRequest(transport, "127.0.0.1");

  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kInternal);
}

TEST(HttpAdapterTest, ProvidesReasonPhrasesForCommonStatusCodes) {
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusCreated), "Created");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusAccepted), "Accepted");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusNoContent), "No Content");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusBadRequest), "Bad Request");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusUnauthorized), "Unauthorized");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusForbidden), "Forbidden");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusNotFound), "Not Found");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusMethodNotAllowed), "Method Not Allowed");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusConflict), "Conflict");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusPayloadTooLarge), "Payload Too Large");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusUnsupportedMediaType),
            "Unsupported Media Type");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusUnprocessableEntity),
            "Unprocessable Entity");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusTooManyRequests), "Too Many Requests");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusInternalServerError),
            "Internal Server Error");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusNotImplemented), "Not Implemented");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusBadGateway), "Bad Gateway");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(a2a::core::http::kStatusServiceUnavailable), "Service Unavailable");
  EXPECT_EQ(a2a::server::HttpAdapter::ReasonPhrase(kHttpUnknown), "Unknown");
}

TEST(HttpAdapterTest, WriteResponseAddsContentLengthAndStatusText) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers[std::string(a2a::core::http::kContentTypeHeaderName)] = std::string(kJsonContentType);
  response.body = std::string(kJsonBody);

  auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);
  ASSERT_TRUE(write.ok());
  EXPECT_NE(transport.output().find(BuildExpectedStatusLine()), std::string::npos);
  EXPECT_NE(transport.output().find(BuildExpectedContentLengthLine()), std::string::npos);
  EXPECT_NE(transport.output().find(kConnectionCloseHeaderLine), std::string::npos);
}

TEST(HttpAdapterTest, WriteResponseRetainsTwoArgumentFunctionSignature) {
  using WriteResponseFunction =
      a2a::core::Result<void> (*)(a2a::server::HttpByteTransport&, const a2a::server::HttpServerResponse&);
  const WriteResponseFunction write_response = &a2a::server::HttpAdapter::WriteResponse;
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.body = std::string(kJsonBody);

  const auto write = write_response(transport, response);

  ASSERT_TRUE(write.ok());
  EXPECT_NE(transport.output().find(kConnectionCloseHeaderLine), std::string::npos);
}

TEST(HttpAdapterTest, WriteResponseCanKeepConnectionAliveExplicitly) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.body = std::string(kJsonBody);

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response, false);

  ASSERT_TRUE(write.ok());
  EXPECT_EQ(transport.output().find(std::string(a2a::core::http::kConnectionHeaderName)), std::string::npos);
}

TEST(HttpAdapterTest, ShouldCloseConnectionHonorsResponseCloseHeader) {
  a2a::server::HttpServerRequest request;
  a2a::server::HttpServerResponse response;
  response.headers[std::string(a2a::core::http::kConnectionHeaderName)] =
      std::string(a2a::core::http::kConnectionCloseHeaderValue);

  EXPECT_TRUE(a2a::server::HttpAdapter::ShouldCloseConnection(request, response));
}

TEST(HttpAdapterTest, ReusableStreamingResponseDoesNotCloseConnection) {
  a2a::server::HttpServerRequest request;
  a2a::server::HttpServerResponse response;
  response.stream_writer = [](a2a::server::HttpByteTransport&) -> a2a::core::Result<void> { return {}; };

  EXPECT_FALSE(a2a::server::HttpAdapter::ShouldCloseConnection(request, response));
}

TEST(HttpAdapterTest, WriteResponseOverridesKeepAliveWhenConnectionMustClose) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.body = std::string(kJsonBody);
  response.headers[std::string(a2a::core::http::kConnectionHeaderName)] =
      std::string(a2a::core::http::kConnectionKeepAliveHeaderValue);

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);

  ASSERT_TRUE(write.ok());
  EXPECT_NE(transport.output().find(kConnectionCloseHeaderLine), std::string::npos);
  EXPECT_EQ(transport.output().find(std::string(a2a::core::http::kConnectionKeepAliveHeaderValue)), std::string::npos);
}

TEST(HttpAdapterTest, WriteResponseRejectsMismatchedContentLength) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers[std::string(a2a::core::http::kContentLengthHeaderName)] = std::string(kMismatchedContentLength);
  response.body = std::string(kJsonBody);

  auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);
  ASSERT_FALSE(write.ok());
  EXPECT_EQ(write.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, WriteResponseHonorsCaseInsensitiveExistingContentLength) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers[std::string(kLowerContentLengthHeaderName)] = std::string(kContentLengthTwo);
  response.body = std::string(kJsonBody);

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);
  ASSERT_TRUE(write.ok());
  EXPECT_EQ(transport.output().find(BuildExpectedContentLengthLine()), std::string::npos);
}

TEST(HttpAdapterTest, WriteResponseRejectsInvalidExistingContentLength) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers[std::string(a2a::core::http::kContentLengthHeaderName)] = std::string(kInvalidContentLength);
  response.body = std::string(kJsonBody);

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);
  ASSERT_FALSE(write.ok());
  EXPECT_EQ(write.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, WriteResponseCompletesPartialWrites) {
  BufferTransport transport("");
  transport.set_partial_write_limit(kPartialWriteLimit);
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.body = std::string(kJsonBody);

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);
  ASSERT_TRUE(write.ok());
  EXPECT_NE(transport.output().find(std::string(kJsonBody)), std::string::npos);
}

TEST(HttpAdapterTest, WriteResponseStreamsBodyWithoutContentLength) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers[std::string(a2a::core::http::kContentTypeHeaderName)] = "text/event-stream";
  response.stream_writer = [](a2a::server::HttpByteTransport& output) -> a2a::core::Result<void> {
    const auto written = output.Write(kSseChunk.data(), kSseChunk.size());
    if (!written.ok()) {
      return written.error();
    }
    if (written.value() != kSseChunk.size()) {
      return a2a::core::Error::Internal("short SSE test write");
    }
    return {};
  };

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);

  ASSERT_TRUE(write.ok());
  EXPECT_NE(transport.output().find(BuildExpectedStatusLine()), std::string::npos);
  EXPECT_EQ(transport.output().find(BuildExpectedContentLengthLine()), std::string::npos);
  EXPECT_NE(transport.output().find(std::string(kSseChunk)), std::string::npos);
}

TEST(HttpAdapterTest, WriteResponseFramesReusableStreamWithChunkedEncoding) {
  BufferTransport transport("");
  transport.set_partial_write_limit(kPartialWriteLimit);
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers[std::string(a2a::core::http::kContentTypeHeaderName)] = "text/event-stream";
  response.stream_writer = [](a2a::server::HttpByteTransport& output) -> a2a::core::Result<void> {
    const auto written = output.Write(kSseChunk.data(), kSseChunk.size());
    if (!written.ok()) {
      return written.error();
    }
    return written.value() == kSseChunk.size() ? a2a::core::Result<void>{}
                                               : a2a::core::Error::Internal("short SSE test write");
  };

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response, false);

  ASSERT_TRUE(write.ok()) << write.error().message();
  EXPECT_NE(transport.output().find(kTransferEncodingChunkedHeaderLine), std::string::npos);
  EXPECT_EQ(transport.output().find(BuildExpectedContentLengthLine()), std::string::npos);
  EXPECT_EQ(transport.output().find(kConnectionCloseHeaderLine), std::string::npos);
  std::string expected_chunk;
  expected_chunk.append(kSseChunkSizeHex);
  expected_chunk.append(a2a::core::http::kLineTerminator);
  expected_chunk.append(kSseChunk);
  expected_chunk.append(a2a::core::http::kLineTerminator);
  expected_chunk.append(kFinalChunk);
  EXPECT_TRUE(transport.output().ends_with(expected_chunk));
}

TEST(HttpAdapterTest, ExplicitCloseStreamingResponseRemainsCloseDelimited) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.stream_writer = [](a2a::server::HttpByteTransport& output) -> a2a::core::Result<void> {
    return output.Write(kSseChunk.data(), kSseChunk.size()).ok() ? a2a::core::Result<void>{}
                                                                 : a2a::core::Error::Internal("SSE write failed");
  };

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response, true);

  ASSERT_TRUE(write.ok());
  EXPECT_NE(transport.output().find(kConnectionCloseHeaderLine), std::string::npos);
  EXPECT_EQ(transport.output().find(kTransferEncodingChunkedHeaderLine), std::string::npos);
  EXPECT_TRUE(transport.output().ends_with(kSseChunk));
}

TEST(HttpAdapterTest, StreamWriterFailureDoesNotWriteSuccessfulFinalChunk) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.stream_writer = [](a2a::server::HttpByteTransport& output) -> a2a::core::Result<void> {
    const auto written = output.Write(kSseChunk.data(), kSseChunk.size());
    if (!written.ok()) {
      return written.error();
    }
    return a2a::core::Error::Internal("stream failed after event");
  };

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response, false);

  ASSERT_FALSE(write.ok());
  EXPECT_FALSE(transport.output().ends_with(kFinalChunk));
}

TEST(HttpAdapterTest, StreamingResponseRejectsCallerTransferEncoding) {
  BufferTransport transport("");
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers[std::string(a2a::core::http::kTransferEncodingHeaderName)] =
      std::string(a2a::core::http::kTransferEncodingChunked);
  response.stream_writer = [](a2a::server::HttpByteTransport&) -> a2a::core::Result<void> { return {}; };

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response, false);

  ASSERT_FALSE(write.ok());
  EXPECT_EQ(write.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, WriteResponseRejectsZeroByteWrites) {
  BufferTransport transport("");
  transport.set_write_zero_bytes(kEnableZeroByteWrites);
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.body = std::string(kJsonBody);

  const auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);
  ASSERT_FALSE(write.ok());
  EXPECT_EQ(write.error().code(), a2a::core::ErrorCode::kInternal);
}

}  // namespace
