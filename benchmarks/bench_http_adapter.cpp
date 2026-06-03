// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include "a2a/core/http_constants.h"
#include "a2a/server/http_adapter.h"
#include "bench_common.h"

namespace {

class MemoryTransport final : public a2a::server::HttpByteTransport {
 public:
  explicit MemoryTransport(std::string input) : input_(std::move(input)) {}

  a2a::core::Result<std::size_t> Read(char* buffer, std::size_t size) override {
    const std::size_t available = input_.size() - read_offset_;
    const std::size_t count = std::min(size, available);
    if (count > 0) {
      std::memcpy(buffer, input_.data() + read_offset_, count);
      read_offset_ += count;
    }
    return count;
  }

  a2a::core::Result<std::size_t> Write(const char* buffer, std::size_t size) override {
    output_.append(buffer, size);
    return size;
  }

  [[nodiscard]] const std::string& output() const noexcept { return output_; }

 private:
  std::string input_;
  std::size_t read_offset_ = 0;
  std::string output_;
};

inline constexpr std::string_view kBody = R"({"hello":"benchmark"})";
inline constexpr std::string_view kTaskPath = "/tasks/task-bench-1";
inline constexpr std::string_view kMessageSendPath = "/message:send";
inline constexpr int kManyHeaderCount = 50;
inline constexpr std::string_view kGetMethod = "GET";
inline constexpr std::string_view kPostMethod = "POST";
inline constexpr std::string_view kLocalhost = "localhost";
inline constexpr std::string_view kHeaderNamePrefix = "X-Bench-Header-";
inline constexpr std::string_view kHeaderValue = "value";
inline constexpr std::size_t kMaxBenchmarkHeaderIndexDigits = 2;
inline constexpr std::size_t kSmallRequestReserveSize = 96;
inline constexpr std::size_t kBodyRequestReserveSize = 128;
inline constexpr std::size_t kManyHeadersReserveSize = 1600;

void AppendRequestLine(std::string* request, std::string_view method, std::string_view target) {
  *request += method;
  request->push_back(' ');
  *request += target;
  request->push_back(' ');
  *request += a2a::core::http::kHttpVersion11;
  *request += a2a::core::http::kLineTerminator;
}

void AppendHeader(std::string* request, std::string_view name, std::string_view value) {
  *request += name;
  *request += a2a::core::http::kHeaderNameValueSeparator;
  *request += value;
  *request += a2a::core::http::kLineTerminator;
}

std::string BuildSmallRequest() {
  std::string request;
  request.reserve(kSmallRequestReserveSize);
  AppendRequestLine(&request, kGetMethod, kTaskPath);
  AppendHeader(&request, a2a::core::http::kHostHeaderName, kLocalhost);
  request += a2a::core::http::kLineTerminator;
  return request;
}

std::string BuildManyHeadersRequest() {
  std::string request;
  request.reserve(kManyHeadersReserveSize);
  AppendRequestLine(&request, kGetMethod, kTaskPath);
  AppendHeader(&request, a2a::core::http::kHostHeaderName, kLocalhost);
  for (int index = 0; index < kManyHeaderCount; ++index) {
    std::string header_name;
    header_name.reserve(kHeaderNamePrefix.size() + kMaxBenchmarkHeaderIndexDigits);
    header_name += kHeaderNamePrefix;
    header_name += std::to_string(index);
    AppendHeader(&request, header_name, kHeaderValue);
  }
  request += a2a::core::http::kLineTerminator;
  return request;
}

std::string BuildBodyRequest() {
  std::string request;
  request.reserve(kBodyRequestReserveSize);
  AppendRequestLine(&request, kPostMethod, kMessageSendPath);
  AppendHeader(&request, a2a::core::http::kHostHeaderName, kLocalhost);
  AppendHeader(&request, a2a::core::http::kContentLengthHeaderName, std::to_string(kBody.size()));
  request += a2a::core::http::kLineTerminator;
  request += kBody;
  return request;
}

void BM_HttpAdapter_ParseSmallRequest(benchmark::State& state) {
  const a2a::server::HttpAdapter adapter;
  for (auto _ : state) {
    MemoryTransport transport{BuildSmallRequest()};
    auto request = adapter.ReadRequest(transport, "127.0.0.1");
    benchmark::DoNotOptimize(request);
  }
}
BENCHMARK(BM_HttpAdapter_ParseSmallRequest);

void BM_HttpAdapter_ParseManyHeaders(benchmark::State& state) {
  const a2a::server::HttpAdapter adapter;
  const std::string raw_request = BuildManyHeadersRequest();
  for (auto _ : state) {
    MemoryTransport transport(raw_request);
    auto request = adapter.ReadRequest(transport, "127.0.0.1");
    benchmark::DoNotOptimize(request);
  }
}
BENCHMARK(BM_HttpAdapter_ParseManyHeaders);

void BM_HttpAdapter_ParseRequestWithBody(benchmark::State& state) {
  const a2a::server::HttpAdapter adapter;
  const std::string raw_request = BuildBodyRequest();
  for (auto _ : state) {
    MemoryTransport transport(raw_request);
    auto request = adapter.ReadRequest(transport, "127.0.0.1");
    benchmark::DoNotOptimize(request);
  }
}
BENCHMARK(BM_HttpAdapter_ParseRequestWithBody);

void BM_HttpAdapter_SerializeResponse(benchmark::State& state) {
  const a2a::server::HttpServerResponse response{
      .status_code = 200, .headers = {{"Content-Type", "text/plain"}}, .body = "benchmark"};
  for (auto _ : state) {
    MemoryTransport transport("");
    auto result = a2a::server::HttpAdapter::WriteResponse(transport, response);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(transport.output());
  }
}
BENCHMARK(BM_HttpAdapter_SerializeResponse);

void BM_HttpAdapter_ContentLengthParsing(benchmark::State& state) {
  const a2a::server::HttpAdapter adapter;
  const std::string raw_request = BuildBodyRequest();
  for (auto _ : state) {
    MemoryTransport transport(raw_request);
    auto request = adapter.ReadRequest(transport, "127.0.0.1");
    benchmark::DoNotOptimize(request);
  }
}
BENCHMARK(BM_HttpAdapter_ContentLengthParsing);

}  // namespace
