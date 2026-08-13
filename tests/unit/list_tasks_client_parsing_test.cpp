// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <array>
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
constexpr std::string_view kRequestId = "parsing-test-request";
constexpr std::string_view kNextPageToken = "parsing-test-next-page";
constexpr std::string_view kTaskIdPrefix = "task-parsing-";
constexpr std::string_view kFirstTaskId = "task-parsing-0";
constexpr std::string_view kContextId = "context-parsing";
constexpr std::string_view kJsonRpcEnvelopePrefix = R"({"jsonrpc":"2.0","id":"parsing-test-request","result":)";
constexpr std::size_t kEmptyTaskCount = 0U;
constexpr std::size_t kSingleTaskCount = 1U;
constexpr std::size_t kTypicalTaskCount = 20U;
constexpr std::size_t kLargeTaskCount = 200U;
constexpr std::array kTaskCounts{kEmptyTaskCount, kSingleTaskCount, kTypicalTaskCount, kLargeTaskCount};

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

lf::a2a::v1::Task BuildTask(std::size_t index) {
  lf::a2a::v1::Task task;
  std::string id(kTaskIdPrefix);
  id.append(std::to_string(index));
  task.set_id(std::move(id));
  task.set_context_id(std::string(kContextId));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_SUBMITTED);
  return task;
}

a2a::core::Result<std::string> BuildListTasksPayload(std::size_t task_count) {
  lf::a2a::v1::ListTasksResponse payload;
  for (std::size_t index = 0U; index < task_count; ++index) {
    *payload.add_tasks() = BuildTask(index);
  }
  payload.set_next_page_token(std::string(kNextPageToken));
  return a2a::core::MessageToJson(payload);
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

void ExpectListTasksFixtureParses(ClientWireFormat format, std::size_t task_count) {
  const auto payload = BuildListTasksPayload(task_count);
  ASSERT_TRUE(payload.ok()) << payload.error().message();

  std::string response_body = payload.value();
  if (format == ClientWireFormat::kJsonRpc) {
    response_body = WrapJsonRpcResult(response_body);
  }

  auto transport = MakeTransport(format, std::move(response_body));
  const auto response = transport->ListTasks({}, {});
  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_EQ(response.value().tasks.size(), task_count);
  EXPECT_EQ(response.value().next_page_token, kNextPageToken);

  if (task_count != 0U) {
    EXPECT_EQ(response.value().tasks.front().id(), kFirstTaskId);
    std::string expected_last_id(kTaskIdPrefix);
    expected_last_id.append(std::to_string(task_count - 1U));
    EXPECT_EQ(response.value().tasks.back().id(), expected_last_id);
  }
}

TEST(ListTasksClientParsingTest, HttpJsonParsesBenchmarkSizedResponsesWithoutNetwork) {
  for (const std::size_t task_count : kTaskCounts) {
    ExpectListTasksFixtureParses(ClientWireFormat::kHttpJson, task_count);
  }
}

TEST(ListTasksClientParsingTest, JsonRpcParsesBenchmarkSizedResponsesWithoutNetwork) {
  for (const std::size_t task_count : kTaskCounts) {
    ExpectListTasksFixtureParses(ClientWireFormat::kJsonRpc, task_count);
  }
}

}  // namespace
