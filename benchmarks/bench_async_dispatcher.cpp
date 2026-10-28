// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <benchmark/benchmark.h>

#include <future>
#include <memory>
#include <utility>

#include "a2a/server/dispatcher.h"
#include "a2a/server/engine/async_dispatcher.h"
#include "bench_common.h"

namespace {
using namespace a2a::server;
using namespace a2a::server::engine;
constexpr auto kDispatchFailed = "Dispatch failed";
constexpr auto kWorkerCounter = "sdk_workers";
// This immediate in-memory fixture measures dispatcher/scheduling overhead only.
// It does not simulate non-blocking application I/O or provide an SDK fallback.
class ImmediateExecutor final : public AsyncAgentExecutor {
 public:
  void SendMessage(lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context,
                   Completion<lf::a2a::v1::SendMessageResponse> completion) override {
    auto metadata = context.Metadata();
    completion(executor_.SendMessage(request, metadata));
  }
  void GetTask(lf::a2a::v1::GetTaskRequest request, RequestExecutionContext context,
               Completion<lf::a2a::v1::Task> completion) override {
    auto metadata = context.Metadata();
    completion(executor_.GetTask(request, metadata));
  }
  void ListTasks(ListTasksRequest request, RequestExecutionContext context,
                 Completion<ListTasksResponse> completion) override {
    auto metadata = context.Metadata();
    completion(executor_.ListTasks(request, metadata));
  }
  void CancelTask(lf::a2a::v1::CancelTaskRequest request, RequestExecutionContext context,
                  Completion<lf::a2a::v1::Task> completion) override {
    auto metadata = context.Metadata();
    completion(executor_.CancelTask(request, metadata));
  }
  void CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request,
                                        RequestExecutionContext context,
                                        Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) override {
    auto metadata = context.Metadata();
    completion(executor_.CreateTaskPushNotificationConfig(request, metadata));
  }
  void GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                     RequestExecutionContext context,
                                     Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) override {
    auto metadata = context.Metadata();
    completion(executor_.GetTaskPushNotificationConfig(request, metadata));
  }
  void ListTaskPushNotificationConfigs(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest request, RequestExecutionContext context,
      Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion) override {
    auto metadata = context.Metadata();
    completion(executor_.ListTaskPushNotificationConfigs(request, metadata));
  }
  void DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
                                        RequestExecutionContext context, Completion<void> completion) override {
    auto metadata = context.Metadata();
    completion(executor_.DeleteTaskPushNotificationConfig(request, metadata));
  }

 private:
  a2a::bench::StaticExecutor executor_;
};
DispatchRequest BuildRequest() {
  lf::a2a::v1::GetTaskRequest query;
  query.set_id(std::string(a2a::bench::kTaskId));
  return {.operation = DispatcherOperation::kGetTask, .payload = std::move(query)};
}
void BM_Dispatcher_SyncImmediate(benchmark::State& state) {
  a2a::bench::StaticExecutor executor;
  Dispatcher dispatcher(&executor);
  const auto request = BuildRequest();
  RequestContext metadata;
  while (state.KeepRunning()) {
    auto response = dispatcher.Dispatch(request, metadata);
    if (!response.ok()) {
      state.SkipWithError(kDispatchFailed);
      break;
    }
    benchmark::DoNotOptimize(response);
  }
  state.counters[kWorkerCounter] = 0;
}
BENCHMARK(BM_Dispatcher_SyncImmediate)->UseRealTime();

void BM_Dispatcher_AsyncImmediate(benchmark::State& state) {
  AsyncDispatcherOptions options;
  options.worker_count = 1;
  auto executor = std::make_shared<ImmediateExecutor>();
  AsyncDispatcher dispatcher(executor, options);
  const auto request = BuildRequest();
  while (state.KeepRunning()) {
    std::promise<bool> reply;
    auto result = reply.get_future();
    const auto admitted = dispatcher.Dispatch(
        request, {},
        [&](auto response, const RequestExecutionContext& /*context*/) { reply.set_value(response.ok()); });
    if (!admitted.ok() || !result.get()) {
      state.SkipWithError(kDispatchFailed);
      break;
    }
  }
  state.counters[kWorkerCounter] = static_cast<double>(options.worker_count);
}
BENCHMARK(BM_Dispatcher_AsyncImmediate)->UseRealTime();
}  // namespace
