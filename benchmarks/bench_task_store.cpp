// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <memory>
#include <string>

#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_history.h"
#include "a2a/server/tasks/task_store.h"
#include "bench_common.h"

namespace {

std::unique_ptr<a2a::server::InMemoryTaskStore> BuildStore(std::size_t count, std::size_t history_count = 1) {
  auto store = std::make_unique<a2a::server::InMemoryTaskStore>();
  for (std::size_t index = 0; index < count; ++index) {
    auto result = store->CreateOrUpdate(a2a::bench::BuildTask("task-" + std::to_string(index), history_count));
    benchmark::DoNotOptimize(result);
  }
  return store;
}

void BM_TaskStore_CreateOrUpdate(benchmark::State& state) {
  std::size_t index = 0;
  for (auto _ : state) {
    state.PauseTiming();
    a2a::server::InMemoryTaskStore store;
    auto task = a2a::bench::BuildTask("task-create-" + std::to_string(index));
    ++index;
    state.ResumeTiming();
    auto result = store.CreateOrUpdate(task);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_TaskStore_CreateOrUpdate);

void BM_TaskStore_Get_ManyTasks(benchmark::State& state) {
  const auto store = BuildStore(a2a::bench::kTaskCount);
  const std::string id = "task-500";
  for (auto _ : state) {
    auto result = store->Get(id);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_TaskStore_Get_ManyTasks);

void BM_TaskStore_List_ManyTasks(benchmark::State& state) {
  const auto store = BuildStore(a2a::bench::kTaskCount);
  const a2a::server::ListTasksRequest request;
  for (auto _ : state) {
    auto result = store->List(request);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_TaskStore_List_ManyTasks);

void BM_TaskStore_AppendTaskHistory_NoDuplicate(benchmark::State& state) {
  std::size_t index = 0;
  for (auto _ : state) {
    state.PauseTiming();
    a2a::server::InMemoryTaskStore store;
    auto create = store.CreateOrUpdate(a2a::bench::BuildTask());
    auto message = a2a::bench::BuildMessage("append-message-" + std::to_string(index));
    ++index;
    benchmark::DoNotOptimize(create);
    state.ResumeTiming();
    auto result =
        store.AppendTaskHistory(a2a::bench::kTaskId, message, a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_TaskStore_AppendTaskHistory_NoDuplicate);

void BM_TaskStore_AppendTaskHistory_DuplicateMessageId(benchmark::State& state) {
  auto store = std::make_unique<a2a::server::InMemoryTaskStore>();
  auto create = store->CreateOrUpdate(a2a::bench::BuildTask());
  benchmark::DoNotOptimize(create);
  const auto message = a2a::bench::BuildMessage("message-history-0");
  for (auto _ : state) {
    auto result = store->AppendTaskHistory(a2a::bench::kTaskId, message,
                                           a2a::server::TaskStore::HistoryAppendPolicy::kDedupByMessageId);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_TaskStore_AppendTaskHistory_DuplicateMessageId);

void BM_TaskStore_AppendTaskHistory_DuplicateFingerprint(benchmark::State& state) {
  auto store = std::make_unique<a2a::server::InMemoryTaskStore>();
  auto create = store->CreateOrUpdate(a2a::bench::BuildTask());
  benchmark::DoNotOptimize(create);
  auto message = a2a::bench::BuildMessage("", a2a::bench::kTaskId, a2a::bench::kContextId);
  auto first =
      store->AppendTaskHistory(a2a::bench::kTaskId, message, a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup);
  benchmark::DoNotOptimize(first);
  for (auto _ : state) {
    auto result = store->AppendTaskHistory(a2a::bench::kTaskId, message,
                                           a2a::server::TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_TaskStore_AppendTaskHistory_DuplicateFingerprint);

void BM_TaskStore_GetTaskHistoryLengthZero(benchmark::State& state) {
  const auto task = a2a::bench::BuildTask(a2a::bench::kTaskId, a2a::bench::kLargeHistoryCount);
  for (auto _ : state) {
    auto projected = task;
    a2a::server::ApplyHistoryRetention(&projected, std::size_t{0});
    benchmark::DoNotOptimize(projected);
  }
}
BENCHMARK(BM_TaskStore_GetTaskHistoryLengthZero);

void BM_TaskStore_GetTaskHistoryLengthOne(benchmark::State& state) {
  const auto task = a2a::bench::BuildTask(a2a::bench::kTaskId, a2a::bench::kLargeHistoryCount);
  for (auto _ : state) {
    auto projected = task;
    a2a::server::ApplyHistoryRetention(&projected, std::size_t{1});
    benchmark::DoNotOptimize(projected);
  }
}
BENCHMARK(BM_TaskStore_GetTaskHistoryLengthOne);

}  // namespace
