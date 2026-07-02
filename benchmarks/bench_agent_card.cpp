// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/protojson.h"
#include "a2a/server/rest_server_transport.h"
#include "bench_common.h"

namespace {

void BM_AgentCardBuilder_Build(benchmark::State& state) {
  for (auto _ : state) {
    auto card = a2a::core::AgentCardBuilder::RestPreset("Benchmark Agent", "http://localhost:8080/a2a").Build();
    benchmark::DoNotOptimize(card);
  }
}
BENCHMARK(BM_AgentCardBuilder_Build);

void BM_AgentCardBuilder_Build3Interfaces(benchmark::State& state) {
  for (auto _ : state) {
    auto card = a2a::core::AgentCardBuilder::ConformancePreset({.rest_url = "http://localhost:8080/a2a",
                                                                .json_rpc_url = "http://localhost:8080/rpc",
                                                                .grpc_url = "localhost:50051"})
                    .Build();
    benchmark::DoNotOptimize(card);
  }
}
BENCHMARK(BM_AgentCardBuilder_Build3Interfaces);

void BM_AgentCardBuilder_Build100Skills(benchmark::State& state) {
  for (auto _ : state) {
    auto card = a2a::bench::BuildAgentCard(1, 100);
    benchmark::DoNotOptimize(card);
  }
}
BENCHMARK(BM_AgentCardBuilder_Build100Skills);

void BM_AgentCard_ToJson(benchmark::State& state) {
  const auto card = a2a::bench::BuildAgentCard(3, 10);
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(card);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_AgentCard_ToJson);

void BM_RestTransport_AgentCardResponse(benchmark::State& state) {
  a2a::bench::StaticExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport transport(&dispatcher, a2a::bench::BuildAgentCard(), {.rest_api_base_path = "/"});
  const auto request = a2a::bench::BuildHttpRequest("GET", "/.well-known/agent-card.json");
  for (auto _ : state) {
    auto response = transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}
BENCHMARK(BM_RestTransport_AgentCardResponse);

}  // namespace
