// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include "a2a/server/json_rpc_server_transport.h"
#include "bench_common.h"

namespace {

inline constexpr std::string_view kSendBody =
    R"({"jsonrpc":"2.0","id":"req-1","method":"message/send","params":{"message":{"messageId":"message-bench-1","taskId":"task-bench-1","contextId":"context-bench-1","role":"ROLE_USER","parts":[{"text":"hello benchmark","mediaType":"text/plain"}]}}})";
inline constexpr std::string_view kGetBody =
    R"({"jsonrpc":"2.0","id":"req-2","method":"tasks/get","params":{"id":"task-bench-1"}})";
inline constexpr std::string_view kListBody =
    R"({"jsonrpc":"2.0","id":"req-3","method":"tasks/list","params":{"pageSize":1}})";
inline constexpr std::string_view kCancelBody =
    R"({"jsonrpc":"2.0","id":"req-4","method":"tasks/cancel","params":{"id":"task-bench-1"}})";
inline constexpr std::string_view kInvalidMethodBody =
    R"({"jsonrpc":"2.0","id":"req-5","method":"tasks/unknown","params":{}})";
inline constexpr std::string_view kInvalidParamsBody =
    R"({"jsonrpc":"2.0","id":"req-6","method":"tasks/get","params":[1,2,3]})";

struct JsonRpcFixture final {
  a2a::bench::StaticExecutor executor;
  a2a::server::Dispatcher dispatcher{&executor};
  a2a::server::JsonRpcServerTransport transport{&dispatcher, {.rpc_path = "/rpc"}};
};

void RunJsonRpcBenchmark(benchmark::State& state, std::string_view body) {
  JsonRpcFixture fixture;
  const auto request = a2a::bench::BuildHttpRequest("POST", "/rpc", std::string(body));
  for (auto _ : state) {
    auto response = fixture.transport.Handle(request);
    benchmark::DoNotOptimize(response);
  }
}

void BM_JsonRpcTransport_SendMessage(benchmark::State& state) { RunJsonRpcBenchmark(state, kSendBody); }
BENCHMARK(BM_JsonRpcTransport_SendMessage);

void BM_JsonRpcTransport_GetTask(benchmark::State& state) { RunJsonRpcBenchmark(state, kGetBody); }
BENCHMARK(BM_JsonRpcTransport_GetTask);

void BM_JsonRpcTransport_ListTasks(benchmark::State& state) { RunJsonRpcBenchmark(state, kListBody); }
BENCHMARK(BM_JsonRpcTransport_ListTasks);

void BM_JsonRpcTransport_CancelTask(benchmark::State& state) { RunJsonRpcBenchmark(state, kCancelBody); }
BENCHMARK(BM_JsonRpcTransport_CancelTask);

void BM_JsonRpcTransport_InvalidMethod(benchmark::State& state) { RunJsonRpcBenchmark(state, kInvalidMethodBody); }
BENCHMARK(BM_JsonRpcTransport_InvalidMethod);

void BM_JsonRpcTransport_InvalidParams(benchmark::State& state) { RunJsonRpcBenchmark(state, kInvalidParamsBody); }
BENCHMARK(BM_JsonRpcTransport_InvalidParams);

void BM_JsonRpcTransport_ErrorEnvelope(benchmark::State& state) { RunJsonRpcBenchmark(state, kInvalidMethodBody); }
BENCHMARK(BM_JsonRpcTransport_ErrorEnvelope);

void BM_JsonRpcTransport_SuccessEnvelope(benchmark::State& state) { RunJsonRpcBenchmark(state, kGetBody); }
BENCHMARK(BM_JsonRpcTransport_SuccessEnvelope);

}  // namespace
