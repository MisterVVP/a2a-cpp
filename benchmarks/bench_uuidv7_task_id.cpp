// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>

#include "a2a/server/task_id_generator.h"
#include "bench_common.h"

namespace {

constexpr int kDefaultBenchmarkThreads = 4;
constexpr std::int64_t kTaskIdGenerationsPerBenchmarkIteration = 1000;

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

template <typename Generator>
void RunTaskIdGeneratorBenchmark(benchmark::State& state, Generator& generator) {
  const auto request = a2a::bench::BuildSendMessageRequest("");
  const a2a::server::RequestContext context;
  for (auto _ : state) {
    for (std::int64_t generation = 0; generation < kTaskIdGenerationsPerBenchmarkIteration; ++generation) {
      auto id = generator.GenerateTaskId(request, context);
      benchmark::DoNotOptimize(id);
    }
  }
  state.SetItemsProcessed(state.iterations() * kTaskIdGenerationsPerBenchmarkIteration);
}

void BM_UuidV7TaskIdGenerator_Generate(benchmark::State& state) {
  a2a::server::UuidV7TaskIdGenerator generator;
  RunTaskIdGeneratorBenchmark(state, generator);
}
BENCHMARK(BM_UuidV7TaskIdGenerator_Generate);

void BM_UuidV7TaskIdGenerator_GenerateMultiThreaded(benchmark::State& state) {
  static a2a::server::UuidV7TaskIdGenerator generator;
  RunTaskIdGeneratorBenchmark(state, generator);
}
BENCHMARK(BM_UuidV7TaskIdGenerator_GenerateMultiThreaded)->Threads(BenchmarkThreadCount());

}  // namespace
