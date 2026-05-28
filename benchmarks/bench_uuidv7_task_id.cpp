// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include "a2a/server/task_id_generator.h"
#include "bench_common.h"

namespace {

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
BENCHMARK(BM_UuidV7TaskIdGenerator_GenerateMultiThreaded)->Threads(4);

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
