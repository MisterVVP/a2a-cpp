// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "a2a/core/json_value.h"

namespace {

constexpr std::string_view kResultMemberName = "result";
constexpr std::string_view kMissingMemberName = "missing";
constexpr std::string_view kEnvelopePrefix = R"({"jsonrpc":"2.0","id":"req-1","result":{"tasks":[)";
constexpr std::string_view kTaskJson =
    R"({"id":"task-1","contextId":"ctx-1","status":{"state":"TASK_STATE_SUBMITTED"},)"
    R"("metadata":{"note":"plain text"}})";
constexpr std::string_view kPageMetadataPrefix = R"(],"pageSize":50,"totalSize":)";
constexpr std::string_view kEnvelopeSuffix = "}}";
constexpr std::size_t kReserveSlackBytes = 32U;
constexpr std::int64_t kOneTask = 1;
constexpr std::int64_t kTypicalTaskCount = 20;
constexpr std::int64_t kLargeTaskCount = 200;
constexpr std::int64_t kVeryLargeTaskCount = 2000;

std::string BuildListTasksEnvelope(std::size_t task_count) {
  const std::size_t separator_count = task_count > 0U ? task_count - 1U : 0U;
  std::string document;
  document.reserve(kEnvelopePrefix.size() + task_count * kTaskJson.size() + separator_count +
                   kPageMetadataPrefix.size() + kEnvelopeSuffix.size() + kReserveSlackBytes);
  document.append(kEnvelopePrefix);
  for (std::size_t index = 0U; index < task_count; ++index) {
    if (index != 0U) {
      document.push_back(',');
    }
    document.append(kTaskJson);
  }
  document.append(kPageMetadataPrefix);
  document.append(std::to_string(task_count));
  document.append(kEnvelopeSuffix);
  return document;
}

void RunFindMemberBenchmark(benchmark::State& state, std::string_view member_name) {
  const auto task_count = static_cast<std::size_t>(state.range(0));
  const std::string document = BuildListTasksEnvelope(task_count);
  for (auto _ : state) {
    const auto range = a2a::core::json::FindTopLevelObjectMemberValue(document, member_name);
    benchmark::DoNotOptimize(range);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(document.size()));
}

void BM_JsonValue_FindResult_ListTasks(benchmark::State& state) { RunFindMemberBenchmark(state, kResultMemberName); }
BENCHMARK(BM_JsonValue_FindResult_ListTasks)
    ->Arg(kOneTask)
    ->Arg(kTypicalTaskCount)
    ->Arg(kLargeTaskCount)
    ->Arg(kVeryLargeTaskCount);

void BM_JsonValue_FindMissing_ListTasks(benchmark::State& state) { RunFindMemberBenchmark(state, kMissingMemberName); }
BENCHMARK(BM_JsonValue_FindMissing_ListTasks)
    ->Arg(kOneTask)
    ->Arg(kTypicalTaskCount)
    ->Arg(kLargeTaskCount)
    ->Arg(kVeryLargeTaskCount);

}  // namespace
