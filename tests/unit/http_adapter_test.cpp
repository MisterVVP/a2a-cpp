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
    request.append(": ");
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
  line.append(" 200 OK");
  line.append(a2a::core::http::kLineTerminator);
  return line;
}

std::string BuildExpectedContentLengthLine() {
  std::string line;
  line.append(a2a::core::http::kContentLengthHeaderName);
  line.append(": 2");
  line.append(a2a::core::http::kLineTerminator);
  return line;
}

TEST(HttpAdapterTest, ParsesContentLengthCaseInsensitive) {
  BufferTransport transport(
      BuildRequest("POST", "/rpc", {{"Host", "localhost"}, {"content-length", "5"}, {"X-Test", "true"}}, kBody));
  const a2a::server::HttpAdapter adapter;
  auto request = adapter.ReadRequest(transport, "127.0.0.1");
  ASSERT_TRUE(request.ok());
  EXPECT_EQ(request.value().method, "POST");
  EXPECT_EQ(request.value().target, "/rpc");
  EXPECT_EQ(request.value().body, kBody);
}

TEST(HttpAdapterTest, RejectsOverflowContentLength) {
  BufferTransport transport(
      BuildRequest("POST", "/rpc", {{"Host", "localhost"}, {"Content-Length", "999999999999999999999999"}}));
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
  EXPECT_NE(transport.output().find(BuildExpectedStatusLine()), std::string::npos);
  EXPECT_NE(transport.output().find(BuildExpectedContentLengthLine()), std::string::npos);
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
