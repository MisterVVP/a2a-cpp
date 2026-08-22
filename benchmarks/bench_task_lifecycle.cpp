// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <memory>
#include <string>

#include "a2a/server/request_context.h"
#include "a2a/server/task_id_generator.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/task_lifecycle_service.h"
#include "bench_common.h"

namespace {

class BenchmarkTaskIdGenerator final : public a2a::server::TaskIdGenerator {
 public:
  [[nodiscard]] a2a::core::Result<std::string> GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                              const a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return std::string(a2a::bench::kTaskId);
  }
};

[[nodiscard]] std::shared_ptr<a2a::server::TaskIdGenerator> MakeBenchmarkTaskIdGenerator() {
  return std::make_shared<BenchmarkTaskIdGenerator>();
}

void BM_TaskLifecycle_CreateNewTask(benchmark::State& state) {
  a2a::server::RequestContext context;
  for (auto _ : state) {
    state.PauseTiming();
    a2a::server::InMemoryTaskStore store;
    a2a::server::TaskLifecycleService service(&store, MakeBenchmarkTaskIdGenerator());
    auto request = a2a::bench::BuildSendMessageRequest("");
    state.ResumeTiming();
    auto id = service.ResolveTaskIdForSendRequest(request, context);
    benchmark::DoNotOptimize(id);
  }
}
BENCHMARK(BM_TaskLifecycle_CreateNewTask);

void BM_TaskLifecycle_ContinueExistingTask(benchmark::State& state) {
  a2a::server::InMemoryTaskStore store;
  const auto create = store.CreateOrUpdate(a2a::bench::BuildTask());
  benchmark::DoNotOptimize(create);
  a2a::server::TaskLifecycleService service(&store, MakeBenchmarkTaskIdGenerator());
  a2a::server::RequestContext context;
  const auto request = a2a::bench::BuildSendMessageRequest();
  for (auto _ : state) {
    const auto id = service.ResolveTaskIdForSendRequest(request, context);
    benchmark::DoNotOptimize(id);
    if (id.ok()) {
      const auto task = store.Get(id.value());
      benchmark::DoNotOptimize(task);
      if (task.ok()) {
        const auto validated = service.ValidateTaskForSendRequest(request, task.value());
        benchmark::DoNotOptimize(validated);
      }
    }
  }
}
BENCHMARK(BM_TaskLifecycle_ContinueExistingTask);

void BM_TaskLifecycle_RejectTerminalTask(benchmark::State& state) {
  a2a::server::InMemoryTaskStore store;
  auto task = a2a::bench::BuildTask();
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  const auto create = store.CreateOrUpdate(task);
  benchmark::DoNotOptimize(create);
  a2a::server::TaskLifecycleService service(&store, MakeBenchmarkTaskIdGenerator());
  a2a::server::RequestContext context;
  const auto request = a2a::bench::BuildSendMessageRequest();
  for (auto _ : state) {
    const auto id = service.ResolveTaskIdForSendRequest(request, context);
    benchmark::DoNotOptimize(id);
    if (id.ok()) {
      const auto current = store.Get(id.value());
      benchmark::DoNotOptimize(current);
      if (current.ok()) {
        const auto validated = service.ValidateTaskForSendRequest(request, current.value());
        benchmark::DoNotOptimize(validated);
      }
    }
  }
}
BENCHMARK(BM_TaskLifecycle_RejectTerminalTask);

void BM_TaskLifecycle_RejectContextMismatch(benchmark::State& state) {
  a2a::server::InMemoryTaskStore store;
  const auto create = store.CreateOrUpdate(a2a::bench::BuildTask());
  benchmark::DoNotOptimize(create);
  a2a::server::TaskLifecycleService service(&store, MakeBenchmarkTaskIdGenerator());
  a2a::server::RequestContext context;
  auto request = a2a::bench::BuildSendMessageRequest();
  request.mutable_message()->set_context_id("different-context");
  for (auto _ : state) {
    const auto id = service.ResolveTaskIdForSendRequest(request, context);
    benchmark::DoNotOptimize(id);
    if (id.ok()) {
      const auto task = store.Get(id.value());
      benchmark::DoNotOptimize(task);
      if (task.ok()) {
        const auto validated = service.ValidateTaskForSendRequest(request, task.value());
        benchmark::DoNotOptimize(validated);
      }
    }
  }
}
BENCHMARK(BM_TaskLifecycle_RejectContextMismatch);

void BM_TaskLifecycle_UpdateStatus(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    a2a::server::InMemoryTaskStore store;
    const auto create = store.CreateOrUpdate(a2a::bench::BuildTask());
    benchmark::DoNotOptimize(create);
    a2a::server::TaskLifecycleService service(&store, MakeBenchmarkTaskIdGenerator());
    state.ResumeTiming();
    auto task = service.TransitionTaskStatus(a2a::bench::kTaskId, lf::a2a::v1::TASK_STATE_WORKING);
    benchmark::DoNotOptimize(task);
  }
}
BENCHMARK(BM_TaskLifecycle_UpdateStatus);

}  // namespace
