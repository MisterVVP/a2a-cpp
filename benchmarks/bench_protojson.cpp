// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include "a2a/core/protojson.h"
#include "a2a/v1/a2a.pb.h"
#include "bench_common.h"

namespace {

void BM_ProtoJson_MessageToJson_SendMessageRequest(benchmark::State& state) {
  const auto request = a2a::bench::BuildSendMessageRequest();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(request);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_MessageToJson_SendMessageRequest);

void BM_ProtoJson_JsonToMessage_SendMessageRequest(benchmark::State& state) {
  const auto json = a2a::bench::JsonOrDie(a2a::bench::BuildSendMessageRequest());
  for (auto _ : state) {
    lf::a2a::v1::SendMessageRequest request;
    auto parse = a2a::core::JsonToMessage(json, &request);
    benchmark::DoNotOptimize(parse);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_ProtoJson_JsonToMessage_SendMessageRequest);

void BM_ProtoJson_MessageToJson_Task(benchmark::State& state) {
  const auto task = a2a::bench::BuildTask();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(task);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_MessageToJson_Task);

void BM_ProtoJson_JsonToMessage_Task(benchmark::State& state) {
  const auto json = a2a::bench::JsonOrDie(a2a::bench::BuildTask());
  for (auto _ : state) {
    lf::a2a::v1::Task task;
    auto parse = a2a::core::JsonToMessage(json, &task);
    benchmark::DoNotOptimize(parse);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_ProtoJson_JsonToMessage_Task);

void BM_ProtoJson_MessageToJson_StreamResponse(benchmark::State& state) {
  const auto response = a2a::bench::BuildStreamResponse();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(response);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_MessageToJson_StreamResponse);

void BM_ProtoJson_JsonToMessage_StreamResponse(benchmark::State& state) {
  const auto json = a2a::bench::JsonOrDie(a2a::bench::BuildStreamResponse());
  for (auto _ : state) {
    lf::a2a::v1::StreamResponse response;
    auto parse = a2a::core::JsonToMessage(json, &response);
    benchmark::DoNotOptimize(parse);
    benchmark::ClobberMemory();
  }
}
BENCHMARK(BM_ProtoJson_JsonToMessage_StreamResponse);

}  // namespace
