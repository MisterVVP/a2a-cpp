// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/protojson.h"
#include "bench_common.h"

namespace {

using a2a::client::ClientTransport;
using a2a::client::HttpClientResponse;
using a2a::client::HttpJsonTransport;
using a2a::client::HttpRequest;
using a2a::client::JsonRpcTransport;
using a2a::client::PreferredTransport;
using a2a::client::ResolvedInterface;

constexpr int kHttpOk = 200;
constexpr std::string_view kRestUrl = "https://agent.example.test/a2a";
constexpr std::string_view kJsonRpcUrl = "https://agent.example.test/rpc";
constexpr std::string_view kRequestId = "benchmark-request";
constexpr std::string_view kNextPageToken = "benchmark-next-page";
constexpr std::string_view kTaskIdPrefix = "task-benchmark-";
constexpr std::string_view kJsonRpcEnvelopePrefix = R"({"jsonrpc":"2.0","id":"benchmark-request","result":)";
constexpr std::string_view kBuildPayloadError = "failed to build ListTasks benchmark payload";
constexpr std::string_view kParseError = "ListTasks client parsing failed";
constexpr std::size_t kNoHistory = 0U;
constexpr std::int64_t kOneTask = 1;
constexpr std::int64_t kTypicalTaskCount = 20;
constexpr std::int64_t kLargeTaskCount = 200;

enum class ClientWireFormat : std::uint8_t {
  kHttpJson,
  kJsonRpc,
};

ResolvedInterface MakeResolvedInterface(ClientWireFormat format) {
  ResolvedInterface resolved;
  resolved.transport = format == ClientWireFormat::kHttpJson ? PreferredTransport::kRest : PreferredTransport::kJsonRpc;
  resolved.url = format == ClientWireFormat::kHttpJson ? std::string(kRestUrl) : std::string(kJsonRpcUrl);
  return resolved;
}

std::string BuildTaskId(std::size_t index) {
  std::string id(kTaskIdPrefix);
  id.append(std::to_string(index));
  return id;
}

std::string BuildListTasksPayload(std::size_t task_count) {
  lf::a2a::v1::ListTasksResponse payload;
  for (std::size_t index = 0U; index < task_count; ++index) {
    *payload.add_tasks() = a2a::bench::BuildTask(BuildTaskId(index), kNoHistory);
  }
  payload.set_next_page_token(std::string(kNextPageToken));

  const auto json = a2a::core::MessageToJson(payload);
  return json.ok() ? json.value() : std::string{};
}

std::string WrapJsonRpcResult(std::string_view payload) {
  std::string envelope;
  envelope.reserve(kJsonRpcEnvelopePrefix.size() + payload.size() + 1U);
  envelope.append(kJsonRpcEnvelopePrefix);
  envelope.append(payload);
  envelope.push_back('}');
  return envelope;
}

std::unique_ptr<ClientTransport> MakeTransport(ClientWireFormat format, std::string response_body) {
  // Keep the network boundary in-memory while preserving the HttpRequester ownership contract.
  // Response materialization is therefore measured together with the real transport parsing path.
  auto requester = [response_body =
                        std::move(response_body)](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
    return HttpClientResponse{.status_code = kHttpOk, .body = response_body};
  };

  if (format == ClientWireFormat::kHttpJson) {
    return std::make_unique<HttpJsonTransport>(MakeResolvedInterface(format), std::move(requester));
  }

  return std::make_unique<JsonRpcTransport>(MakeResolvedInterface(format), std::move(requester),
                                            JsonRpcTransport::kDefaultTimeout, [] { return std::string(kRequestId); });
}

void RunListTasksClientBenchmark(benchmark::State& state, ClientWireFormat format) {
  const auto task_count = static_cast<std::size_t>(state.range(0));
  std::string response_body = BuildListTasksPayload(task_count);
  if (response_body.empty()) {
    state.SkipWithError(kBuildPayloadError.data());
    return;
  }
  if (format == ClientWireFormat::kJsonRpc) {
    response_body = WrapJsonRpcResult(response_body);
  }

  const auto response_size = static_cast<std::int64_t>(response_body.size());
  auto transport = MakeTransport(format, std::move(response_body));

  for (auto _ : state) {
    auto response = transport->ListTasks({}, {});
    if (!response.ok()) {
      state.SkipWithError(kParseError.data());
      return;
    }
    benchmark::DoNotOptimize(response.value());
  }

  state.SetBytesProcessed(state.iterations() * response_size);
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(task_count));
}

void BM_HttpJsonClient_ListTasks_Parse(benchmark::State& state) {
  RunListTasksClientBenchmark(state, ClientWireFormat::kHttpJson);
}
BENCHMARK(BM_HttpJsonClient_ListTasks_Parse)->Arg(kOneTask)->Arg(kTypicalTaskCount)->Arg(kLargeTaskCount);

void BM_JsonRpcClient_ListTasks_Parse(benchmark::State& state) {
  RunListTasksClientBenchmark(state, ClientWireFormat::kJsonRpc);
}
BENCHMARK(BM_JsonRpcClient_ListTasks_Parse)->Arg(kOneTask)->Arg(kTypicalTaskCount)->Arg(kLargeTaskCount);

}  // namespace
