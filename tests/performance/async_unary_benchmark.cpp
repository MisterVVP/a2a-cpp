// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/create_channel.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "a2a/client/async_client.h"
#include "a2a/client/async_transport.h"
#include "a2a/client/grpc_transport.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::string_view kGrpc = "grpc";
constexpr std::string_view kHttpJson = "http_json";
constexpr std::string_view kJsonRpc = "jsonrpc";
constexpr std::string_view kSync = "sync";
constexpr std::string_view kAsync = "async";
constexpr auto kWait = std::chrono::seconds(30);
constexpr std::size_t kPercentile50 = 50;
constexpr std::size_t kPercentile95 = 95;
constexpr std::size_t kPercentile99 = 99;
constexpr std::size_t kPercentile100 = 100;
constexpr std::size_t kMaxConcurrency = 512;
constexpr auto kRequiredExtension = "urn:a2a:tck:required-extension";
constexpr auto kTaskDirectory = "/proc/self/task";
constexpr auto kUsage =
    "usage: async_unary_benchmark grpc|http_json|jsonrpc ENDPOINT sync|async REQUESTS CONCURRENCY\n";
constexpr auto kTransportPrefix = R"({"transport":")";
constexpr auto kModePrefix = R"(","mode":")";
constexpr auto kConcurrencyPrefix = R"(","concurrency":)";
constexpr auto kRequestsPrefix = ",\"requests\":";
constexpr auto kErrorsPrefix = ",\"errors\":";
constexpr auto kThroughputPrefix = ",\"requests_per_second\":";
constexpr auto kP50Prefix = ",\"p50_ms\":";
constexpr auto kP95Prefix = ",\"p95_ms\":";
constexpr auto kP99Prefix = ",\"p99_ms\":";
constexpr auto kThreadCountPrefix = ",\"peak_sampled_client_threads\":";
constexpr auto kReportEnd = "}\n";
constexpr int kArguments = 6;
constexpr int kTransportArgument = 1;
constexpr int kEndpointArgument = 2;
constexpr int kModeArgument = 3;
constexpr std::size_t kThreadSampleInterval = 16;
constexpr int kRequestsArgument = 4;
constexpr int kConcurrencyArgument = 5;

std::size_t ParseCount(const char* value) {
  const std::string_view text(value);
  std::size_t count = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? count : 0U;
}
std::size_t Threads() {
#if defined(__linux__)
  return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(kTaskDirectory), {}));
#else
  return 0U;
#endif
}
std::unique_ptr<a2a::client::ClientTransport> MakeTransport(std::string_view transport, const char* endpoint) {
  a2a::client::ResolvedInterface resolved;
  resolved.url = endpoint;
  if (transport == kGrpc) {
    resolved.transport = a2a::client::PreferredTransport::kGrpc;
    return std::make_unique<a2a::client::GrpcTransport>(
        resolved, ::grpc::CreateChannel(endpoint, ::grpc::InsecureChannelCredentials()));
  }
  if (transport == kHttpJson) {
    resolved.transport = a2a::client::PreferredTransport::kRest;
    return a2a::client::HttpJsonTransport::CreateDefault(resolved);
  }
  resolved.transport = a2a::client::PreferredTransport::kJsonRpc;
  return a2a::client::JsonRpcTransport::CreateDefault(resolved);
}

class Measurement final {
 public:
  Measurement(a2a::client::A2AClient& client, std::size_t requests, const a2a::client::CallOptions& options)
      : client_(client), options_(options), latencies_(requests) {}
  void RunSync() {
    while (const auto index = Next()) {
      const auto started = Clock::now();
      Record(*index, started, client_.ListTasks({}, options_).ok());
    }
  }
  void StartAsync() {
    while (true) {
      const auto index = Next();
      if (!index) {
        return;
      }
      const auto started = Clock::now();
      auto accepted = client_.Async().ListTasks(
          {},
          [this, index = *index, started](auto result) {
            Record(index, started, result.ok());
            StartAsync();
          },
          options_);
      if (accepted.ok()) {
        return;
      }
      Record(*index, started, false);
    }
  }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kWait, [this] { return completed_ == latencies_.size(); });
  }
  void Report(std::string_view transport, std::string_view mode, std::size_t concurrency, double seconds) {
    std::ranges::sort(latencies_);
    const auto percentile = [this](std::size_t percent) {
      return latencies_.at((latencies_.size() - 1U) * percent / kPercentile100);
    };
    std::cout << kTransportPrefix << transport << kModePrefix << mode << kConcurrencyPrefix << concurrency
              << kRequestsPrefix << latencies_.size() << kErrorsPrefix << errors_ << kThroughputPrefix
              << static_cast<double>(latencies_.size()) / seconds << kP50Prefix << percentile(kPercentile50)
              << kP95Prefix << percentile(kPercentile95) << kP99Prefix << percentile(kPercentile99)
              << kThreadCountPrefix << peak_threads_ << kReportEnd;
  }
  [[nodiscard]] bool ok() const { return errors_ == 0; }

 private:
  std::optional<std::size_t> Next() {
    const auto index = next_.fetch_add(1U);
    return index < latencies_.size() ? std::optional(index) : std::nullopt;
  }
  void Record(std::size_t index, Clock::time_point started, bool ok) {
    const auto latency = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    std::lock_guard lock(mutex_);
    latencies_[index] = latency;
    errors_ += ok ? 0U : 1U;
    if (completed_ % kThreadSampleInterval == 0U) {
      peak_threads_ = std::max(peak_threads_, Threads());
    }
    ++completed_;
    changed_.notify_all();
  }
  a2a::client::A2AClient& client_;
  const a2a::client::CallOptions& options_;
  std::vector<double> latencies_;
  std::atomic<std::size_t> next_{0U};
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t completed_ = 0U;
  std::size_t errors_ = 0U;
  std::size_t peak_threads_ = 0U;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != kArguments) {
    std::cerr << kUsage;
    return 1;
  }
  const std::string_view transport(argv[kTransportArgument]);
  const std::string_view mode(argv[kModeArgument]);
  const auto requests = ParseCount(argv[kRequestsArgument]);
  const auto concurrency = ParseCount(argv[kConcurrencyArgument]);
  if ((transport != kGrpc && transport != kHttpJson && transport != kJsonRpc) || (mode != kSync && mode != kAsync) ||
      requests == 0U || concurrency == 0U || concurrency > kMaxConcurrency) {
    std::cerr << kUsage;
    return 1;
  }
  a2a::client::A2AClient client(MakeTransport(transport, argv[kEndpointArgument]));
  // Warm the channel/cache equally before measuring either execution mode.
  a2a::client::CallOptions options;
  options.timeout = kWait;
  options.extensions = {kRequiredExtension};
  const auto warmup = client.ListTasks({}, options);
  if (!warmup.ok()) {
    std::cerr << warmup.error().message() << '\n';
    return 1;
  }
  Measurement measurement(client, requests, options);
  const auto started = Clock::now();
  std::vector<std::thread> workers;
  for (std::size_t index = 0; index < std::min(concurrency, requests); ++index) {
    if (mode == kSync) {
      workers.emplace_back([&] { measurement.RunSync(); });
    } else {
      measurement.StartAsync();
    }
  }
  for (auto& worker : workers) {
    worker.join();
  }
  const bool finished = measurement.Wait();
  const auto elapsed = std::chrono::duration<double>(Clock::now() - started).count();
  (void)client.Destroy();
  if (!finished) {
    return 1;
  }
  measurement.Report(transport, mode, concurrency, elapsed);
  return measurement.ok() ? 0 : 1;
}
