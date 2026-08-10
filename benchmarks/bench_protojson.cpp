// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <cstdint>
#include <string>

#include "a2a/core/protojson.h"
#include "a2a/v1/a2a.pb.h"
#include "bench_common.h"

namespace {

constexpr std::int32_t kSingleTask = 1;
constexpr std::int64_t kSmallTaskList = 10;
constexpr std::int64_t kMediumTaskList = 50;
constexpr std::string_view kTaskIdPrefix = "task-";

lf::a2a::v1::SendMessageResponse BuildSendMessageResponse() {
  lf::a2a::v1::SendMessageResponse response;
  *response.mutable_task() = a2a::bench::BuildTask();
  return response;
}

lf::a2a::v1::TaskPushNotificationConfig BuildPushConfigResponse() {
  lf::a2a::v1::TaskPushNotificationConfig response;
  response.set_id(std::string(a2a::bench::kPushConfigId));
  response.set_task_id(std::string(a2a::bench::kTaskId));
  response.set_url(std::string(a2a::bench::kPushUrl));
  return response;
}

lf::a2a::v1::ListTaskPushNotificationConfigsResponse BuildPushConfigListResponse() {
  lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
  *response.add_configs() = BuildPushConfigResponse();
  return response;
}

lf::a2a::v1::ListTasksResponse BuildListTasksResponse(std::int32_t task_count) {
  lf::a2a::v1::ListTasksResponse response;
  response.mutable_tasks()->Reserve(task_count);
  for (std::int32_t index = 0; index < task_count; ++index) {
    std::string task_id(kTaskIdPrefix);
    task_id.append(std::to_string(index));
    *response.add_tasks() = a2a::bench::BuildTask(task_id);
  }
  response.set_page_size(task_count);
  response.set_total_size(task_count);
  return response;
}

void BM_ProtoJson_MessageToJson_SendMessageRequest(benchmark::State& state) {
  const auto request = a2a::bench::BuildSendMessageRequest();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(request);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_MessageToJson_SendMessageRequest);

void BM_ProtoJson_ResponseToJson_SendMessage(benchmark::State& state) {
  const auto response = BuildSendMessageResponse();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(response);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_ResponseToJson_SendMessage);

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

void BM_ProtoJson_ResponseToJson_Task(benchmark::State& state) {
  const auto task = a2a::bench::BuildTask();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(task);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_ResponseToJson_Task);

void BM_ProtoJson_ResponseToJson_TaskList(benchmark::State& state) {
  const auto response = BuildListTasksResponse(static_cast<std::int32_t>(state.range(0)));
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(response);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_ResponseToJson_TaskList)->Arg(kSingleTask)->Arg(kSmallTaskList)->Arg(kMediumTaskList);

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

void BM_ProtoJson_ResponseToJson_Stream(benchmark::State& state) {
  const auto response = a2a::bench::BuildStreamResponse();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(response);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_ResponseToJson_Stream);

void BM_ProtoJson_ResponseToJson_PushConfig(benchmark::State& state) {
  const auto response = BuildPushConfigResponse();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(response);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_ResponseToJson_PushConfig);

void BM_ProtoJson_ResponseToJson_PushConfigList(benchmark::State& state) {
  const auto response = BuildPushConfigListResponse();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(response);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_ResponseToJson_PushConfigList);

void BM_ProtoJson_ResponseToJson_AgentCard(benchmark::State& state) {
  const auto response = a2a::bench::BuildAgentCard();
  for (auto _ : state) {
    auto json = a2a::core::MessageToJson(response);
    benchmark::DoNotOptimize(json);
  }
}
BENCHMARK(BM_ProtoJson_ResponseToJson_AgentCard);

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
