// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/core/protojson.h"
#include "google/protobuf/struct.pb.h"
#include "support/async_unary_test_support.h"

namespace {
using namespace a2a::test;
using Clock = std::chrono::steady_clock;
constexpr std::size_t kDefaultRequests = 32;
constexpr std::size_t kMaximumRequests = 128;
constexpr std::size_t kWorkers = 2;
constexpr auto kBackendHold = std::chrono::milliseconds(10);
constexpr auto kSync = "sync";
constexpr auto kAsync = "async";
constexpr auto kMode = "mode";
constexpr auto kRequests = "requests";
constexpr auto kAppThreads = "application_threads";
constexpr auto kOsBaseline = "process_threads_before";
constexpr auto kOsPending = "process_threads_pending";
constexpr auto kLatencyMedian = "latency_p50_us";
constexpr auto kLatencyTail = "latency_p95_us";
constexpr auto kHold = "backend_hold_ms";
constexpr auto kFailed = "Async gRPC evidence failed\n";
#if defined(__linux__)
constexpr auto kThreadStatus = "/proc/self/status";
constexpr auto kThreadField = "Threads:";
#endif
constexpr auto kNull = google::protobuf::NULL_VALUE;
constexpr double kMedian = 0.5;
constexpr double kTail = 0.95;

std::optional<std::size_t> ProcessThreads() {
#if defined(__linux__)
  std::ifstream status(kThreadStatus);
  std::string field;
  while (status >> field) {
    if (field == kThreadField) {
      std::size_t threads = 0;
      status >> threads;
      return threads;
    }
    std::string rest;
    std::getline(status, rest);
  }
#endif
  return std::nullopt;
}

struct Request final : std::enable_shared_from_this<Request> {
  explicit Request() : future(done.get_future()) {
    query.set_id(kAsyncTaskId);
    PrepareGrpcClient(context);
  }
  void Start(UnaryGrpcServer& server) {
    started = Clock::now();
    server.Client().GetTask(server.Stub(), context, query, response, [self = shared_from_this()](auto status) {
      self->latency = std::chrono::duration<double, std::micro>(Clock::now() - self->started).count();
      self->done.set_value(status.ok());
    });
  }
  ::grpc::ClientContext context;
  lf::a2a::v1::GetTaskRequest query;
  lf::a2a::v1::Task response;
  std::promise<bool> done;
  std::future<bool> future;
  Clock::time_point started;
  double latency = 0;
};

void Number(google::protobuf::Struct& row, const char* key, double value) {
  (*row.mutable_fields())[key].set_number_value(value);
}
void ThreadMetric(google::protobuf::Struct& row, const char* key, std::optional<std::size_t> count) {
  if (count.has_value()) {
    Number(row, key, static_cast<double>(*count));
  } else {
    (*row.mutable_fields())[key].set_null_value(kNull);
  }
}

std::optional<google::protobuf::Struct> Measure(bool asynchronous, std::size_t count, std::chrono::milliseconds hold) {
  auto backend = std::make_shared<UnaryBackend>(hold > std::chrono::milliseconds::zero());
  a2a::server::engine::AsyncDispatcherOptions options;
  UnaryGrpcServer server(backend, asynchronous, {}, options);
  if (!server.Ready()) {
    return std::nullopt;
  }
  // Warm both gRPC paths without entering the controlled application backend.
  ::grpc::ClientContext warmup;
  PrepareGrpcClient(warmup);
  lf::a2a::v1::GetExtendedAgentCardRequest card_query;
  lf::a2a::v1::AgentCard card;
  if (!server.Stub().GetExtendedAgentCard(&warmup, card_query, &card).ok()) {
    return std::nullopt;
  }
  const auto before = ProcessThreads();
  std::vector<std::shared_ptr<Request>> requests;
  requests.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    auto request = std::make_shared<Request>();
    request->Start(server);
    requests.push_back(std::move(request));
  }
  const bool received = backend->WaitForCalls(count);
  const auto pending = ProcessThreads();
  const auto workers = backend->WorkerCount();
  // This delay models an external backend readiness event; it sleeps only the
  // benchmark controller, never an async dispatcher or gRPC callback worker.
  std::this_thread::sleep_for(hold);
  backend->Release();
  std::vector<double> latencies;
  latencies.reserve(count);
  for (const auto& request : requests) {
    if (request->future.wait_for(kAsyncWait) != std::future_status::ready || !request->future.get()) {
      return std::nullopt;
    }
    latencies.push_back(request->latency);
  }
  if (!received || (asynchronous && workers > kWorkers)) {
    return std::nullopt;
  }
  std::ranges::sort(latencies);
  google::protobuf::Struct row;
  (*row.mutable_fields())[kMode].set_string_value(asynchronous ? kAsync : kSync);
  Number(row, kRequests, static_cast<double>(count));
  Number(row, kAppThreads, static_cast<double>(workers));
  Number(row, kHold, static_cast<double>(hold.count()));
  Number(row, kLatencyMedian, latencies.at(static_cast<std::size_t>(static_cast<double>(count - 1) * kMedian)));
  Number(row, kLatencyTail, latencies.at(static_cast<std::size_t>(static_cast<double>(count - 1) * kTail)));
  ThreadMetric(row, kOsBaseline, before);
  ThreadMetric(row, kOsPending, pending);
  return row;
}
}  // namespace

int main(int argc, char** argv) {
  std::size_t count = kDefaultRequests;
  auto hold = kBackendHold;
  if (argc > 1) {
    const std::string_view argument(argv[1]);
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || count == 0 ||
        count > kMaximumRequests) {
      return EXIT_FAILURE;
    }
  }
  if (argc > 2) {
    std::chrono::milliseconds::rep delay = 0;
    const std::string_view argument(argv[2]);
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), delay);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || delay < 0 ||
        delay > kBackendHold.count()) {
      return EXIT_FAILURE;
    }
    hold = std::chrono::milliseconds(delay);
  }
  google::protobuf::ListValue results;
  for (bool asynchronous : {false, true}) {
    auto row = Measure(asynchronous, count, hold);
    if (!row.has_value()) {
      std::cerr << kFailed;
      return EXIT_FAILURE;
    }
    *results.add_values()->mutable_struct_value() = std::move(*row);
  }
  const auto json = a2a::core::MessageToJson(results);
  if (!json.ok()) {
    std::cerr << kFailed;
    return EXIT_FAILURE;
  }
  std::cout << json.value() << '\n';
  return EXIT_SUCCESS;
}
