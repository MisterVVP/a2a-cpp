// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <cstdlib>

#include "a2a/server/task_id_generator.h"
#include "bench_common.h"

namespace {

constexpr int kDefaultBenchmarkThreads = 4;

int BenchmarkThreadCount() {
  const char* value = std::getenv("A2A_BENCHMARK_THREADS");
  if (value == nullptr || *value == '\0') {
    return kDefaultBenchmarkThreads;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed <= 0) {
    return kDefaultBenchmarkThreads;
  }
  return static_cast<int>(parsed);
}

void BM_UuidV7TaskIdGenerator_Generate(benchmark::State& state) {
  a2a::server::UuidV7TaskIdGenerator generator;
  const auto request = a2a::bench::BuildSendMessageRequest("");
  const a2a::server::RequestContext context;
  for (auto _ : state) {
    auto id = generator.GenerateTaskId(request, context);
    benchmark::DoNotOptimize(id);
  }
}
BENCHMARK(BM_UuidV7TaskIdGenerator_Generate);

void BM_UuidV7TaskIdGenerator_GenerateMultiThreaded(benchmark::State& state) {
  static a2a::server::UuidV7TaskIdGenerator generator;
  const auto request = a2a::bench::BuildSendMessageRequest("");
  const a2a::server::RequestContext context;
  for (auto _ : state) {
    auto id = generator.GenerateTaskId(request, context);
    benchmark::DoNotOptimize(id);
  }
}
BENCHMARK(BM_UuidV7TaskIdGenerator_GenerateMultiThreaded)->Threads(BenchmarkThreadCount());

void BM_SequentialTaskIdGenerator_Generate(benchmark::State& state) {
  a2a::server::SequentialTaskIdGenerator generator;
  const auto request = a2a::bench::BuildSendMessageRequest("");
  const a2a::server::RequestContext context;
  for (auto _ : state) {
    auto id = generator.GenerateTaskId(request, context);
    benchmark::DoNotOptimize(id);
  }
}
BENCHMARK(BM_SequentialTaskIdGenerator_Generate);

}  // namespace
