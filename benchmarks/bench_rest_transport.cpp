// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include "a2a/server/rest_server_transport.h"
#include "bench_common.h"

namespace {

struct RestFixture final {
  a2a::bench::StaticExecutor executor;
  a2a::server::Dispatcher dispatcher{&executor};
  a2a::server::RestServerTransport transport{&dispatcher, a2a::bench::BuildAgentCard(), {.rest_api_base_path = "/"}};
  std::string send_body = a2a::bench::JsonOrDie(a2a::bench::BuildSendMessageRequest());
};

void BM_RestTransport_SendMessage(benchmark::State& state) {
  RestFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("POST", "/message:send", fixture.send_body);
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_SendMessage);

void BM_RestTransport_GetTask(benchmark::State& state) {
  RestFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("GET", "/tasks/task-bench-1");
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_GetTask);

void BM_RestTransport_GetTaskHistoryLengthZero(benchmark::State& state) {
  RestFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("GET", "/tasks/task-bench-1?historyLength=0");
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_GetTaskHistoryLengthZero);

void BM_RestTransport_GetTaskHistoryLengthOne(benchmark::State& state) {
  RestFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("GET", "/tasks/task-bench-1?historyLength=1");
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_GetTaskHistoryLengthOne);

void BM_RestTransport_ListTasks(benchmark::State& state) {
  RestFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("GET", "/tasks");
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_ListTasks);

void BM_RestTransport_CancelTask(benchmark::State& state) {
  RestFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("POST", "/tasks/task-bench-1:cancel");
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_CancelTask);

void BM_RestTransport_InvalidPath(benchmark::State& state) {
  RestFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("GET", "/tasks/task-bench-1/invalid");
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_InvalidPath);

void BM_RestTransport_QueryParsing(benchmark::State& state) {
  RestFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("GET", "/tasks?pageSize=1&pageToken=0&historyLength=1");
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_QueryParsing);

}  // namespace
