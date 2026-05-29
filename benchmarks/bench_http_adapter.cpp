// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

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

inline constexpr std::string_view kSmallRequest = "GET /tasks/task-bench-1 HTTP/1.1\r\nHost: localhost\r\n\r\n";
inline constexpr std::string_view kBody = R"({"hello":"benchmark"})";

std::string BuildManyHeadersRequest() {
  std::string request = "GET /tasks/task-bench-1 HTTP/1.1\r\nHost: localhost\r\n";
  for (int index = 0; index < 50; ++index) {
    request += "X-Bench-Header-" + std::to_string(index) + ": value\r\n";
  }
  request += "\r\n";
  return request;
}

std::string BuildBodyRequest() {
  return "POST /message:send HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(kBody.size()) +
         "\r\n\r\n" + std::string(kBody);
}

void BM_HttpAdapter_ParseSmallRequest(benchmark::State& state) {
  const a2a::server::HttpAdapter adapter;
  for (auto _ : state) {
    MemoryTransport transport{std::string(kSmallRequest)};
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
