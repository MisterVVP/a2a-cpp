// SPDX-License-Identifier: Apache-2.0

#include "a2a/server/http_adapter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "a2a/core/error.h"

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
    output_.append(buffer, size);
    return size;
  }

  [[nodiscard]] const std::string& output() const { return output_; }

 private:
  std::string input_;
  std::size_t read_offset_ = 0;
  std::string output_;
};

constexpr std::string_view kBody = "hello";
constexpr int kHttpOk = 200;

TEST(HttpAdapterTest, ParsesContentLengthCaseInsensitive) {
  BufferTransport transport("POST /rpc HTTP/1.1\r\nHost: localhost\r\ncontent-length: 5\r\nX-Test: true\r\n\r\nhello");
  const a2a::server::HttpAdapter adapter;
  auto request = adapter.ReadRequest(transport, "127.0.0.1");
  ASSERT_TRUE(request.ok());
  EXPECT_EQ(request.value().method, "POST");
  EXPECT_EQ(request.value().target, "/rpc");
  EXPECT_EQ(request.value().body, kBody);
}

TEST(HttpAdapterTest, RejectsOverflowContentLength) {
  BufferTransport transport(
      "POST /rpc HTTP/1.1\r\nHost: localhost\r\nContent-Length: 999999999999999999999999\r\n\r\n");
  const a2a::server::HttpAdapter adapter;
  auto request = adapter.ReadRequest(transport, "127.0.0.1");
  ASSERT_FALSE(request.ok());
  EXPECT_EQ(request.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpAdapterTest, WriteResponseAddsContentLengthAndStatusText) {
  BufferTransport transport("");
  const a2a::server::HttpAdapter adapter;
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers["Content-Type"] = "application/json";
  response.body = "{}";

  auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);
  ASSERT_TRUE(write.ok());
  EXPECT_NE(transport.output().find("HTTP/1.1 200 OK\r\n"), std::string::npos);
  EXPECT_NE(transport.output().find("Content-Length: 2\r\n"), std::string::npos);
}

TEST(HttpAdapterTest, WriteResponseRejectsMismatchedContentLength) {
  BufferTransport transport("");
  const a2a::server::HttpAdapter adapter;
  a2a::server::HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers["Content-Length"] = "99";
  response.body = "{}";

  auto write = a2a::server::HttpAdapter::WriteResponse(transport, response);
  ASSERT_FALSE(write.ok());
  EXPECT_EQ(write.error().code(), a2a::core::ErrorCode::kValidation);
}

}  // namespace
