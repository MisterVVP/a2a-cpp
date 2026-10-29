// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <benchmark/benchmark.h>

#include <boost/asio/thread_pool.hpp>
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
constexpr std::size_t kIoWorkers = 1;
constexpr auto kWorkerCounter = "asio_workers";
// This immediate in-memory fixture measures dispatcher/scheduling overhead only.
// It does not simulate non-blocking application I/O or provide an SDK fallback.
class ImmediateExecutor final : public CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::SendMessageResponse>> SendMessage(
      lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context) override {
    auto metadata = context.Metadata();
    co_return executor_.SendMessage(request, metadata);
  }
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::Task>> GetTask(lf::a2a::v1::GetTaskRequest request,
                                                                       RequestExecutionContext context) override {
    auto metadata = context.Metadata();
    co_return executor_.GetTask(request, metadata);
  }
  boost::asio::awaitable<a2a::core::Result<ListTasksResponse>> ListTasks(ListTasksRequest request,
                                                                         RequestExecutionContext context) override {
    auto metadata = context.Metadata();
    co_return executor_.ListTasks(request, metadata);
  }
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::Task>> CancelTask(lf::a2a::v1::CancelTaskRequest request,
                                                                          RequestExecutionContext context) override {
    auto metadata = context.Metadata();
    co_return executor_.CancelTask(request, metadata);
  }
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>> CreateTaskPushNotificationConfig(
      lf::a2a::v1::TaskPushNotificationConfig request, RequestExecutionContext context) override {
    auto metadata = context.Metadata();
    co_return executor_.CreateTaskPushNotificationConfig(request, metadata);
  }
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>> GetTaskPushNotificationConfig(
      lf::a2a::v1::GetTaskPushNotificationConfigRequest request, RequestExecutionContext context) override {
    auto metadata = context.Metadata();
    co_return executor_.GetTaskPushNotificationConfig(request, metadata);
  }
  boost::asio::awaitable<a2a::core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
  ListTaskPushNotificationConfigs(lf::a2a::v1::ListTaskPushNotificationConfigsRequest request,
                                  RequestExecutionContext context) override {
    auto metadata = context.Metadata();
    co_return executor_.ListTaskPushNotificationConfigs(request, metadata);
  }
  boost::asio::awaitable<a2a::core::Result<void>> DeleteTaskPushNotificationConfig(
      lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request, RequestExecutionContext context) override {
    auto metadata = context.Metadata();
    co_return executor_.DeleteTaskPushNotificationConfig(request, metadata);
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
  boost::asio::thread_pool runtime(kIoWorkers);
  auto executor = std::make_shared<ImmediateExecutor>();
  AsyncDispatcher dispatcher(executor, runtime.get_executor(), options);
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
  state.counters[kWorkerCounter] = static_cast<double>(kIoWorkers);
}
BENCHMARK(BM_Dispatcher_AsyncImmediate)->UseRealTime();
}  // namespace
