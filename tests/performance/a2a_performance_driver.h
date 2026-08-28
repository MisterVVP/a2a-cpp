// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <google/protobuf/struct.pb.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__cpp_lib_jthread) && (__cpp_lib_jthread >= 201911L)
#define A2A_PERF_HAS_JTHREAD 1
#else
#define A2A_PERF_HAS_JTHREAD 0
#endif

#include "a2a/v1/a2a.pb.h"

namespace a2a::tests::performance {

constexpr std::string_view kGrpcTransport = "grpc";
constexpr std::string_view kJsonRpcTransport = "jsonrpc";
constexpr std::string_view kHttpJsonTransport = "http_json";
constexpr std::string_view kInMemoryStore = "inmemory";
constexpr std::string_view kPostgresStore = "postgres";
constexpr std::string_view kDriverType = "cpp_sdk_in_process";
constexpr double kNanosecondsPerMillisecond = 1000000.0;
constexpr double kMinElapsedSeconds = 0.000001;
constexpr int kDefaultRequests = 1000;
constexpr int kDefaultConcurrency = 1;
constexpr int kListPageSize = 10;
constexpr int kPushConfigFanout = 8;
constexpr int kFollowUpHistoryDepth = 1;
constexpr int kDeepFollowUpHistoryDepth = 8;
constexpr int kMultiSubscriberCount = 3;
constexpr int kDisconnectSubscriberCount = 2;
constexpr int kHttpStatusOk = 200;
constexpr int kUsageExitCode = 2;
constexpr double kP50 = 50.0;
constexpr double kP90 = 90.0;
constexpr double kP95 = 95.0;
constexpr double kP99 = 99.0;
constexpr std::size_t kIdReserveSlack = 16U;
constexpr std::size_t kPostgresDiagnosticPhaseCount = 12U;
constexpr std::array<std::string_view, kPostgresDiagnosticPhaseCount> kPostgresDiagnosticPhaseNames = {
    "connection_acquire_wait",
    "task_get",
    "task_upsert",
    "task_history_snapshot",
    "task_history_lock_read",
    "push_config_upsert",
    "push_config_get",
    "push_config_delete",
    "push_config_list_count",
    "push_config_list_select",
    "transaction_begin",
    "transaction_commit"};
constexpr char kPostgresDsnEnv[] = "A2A_TEST_POSTGRES_DSN";
constexpr char kPostgresSchemaEnv[] = "A2A_PERF_POSTGRES_SCHEMA";
constexpr char kPostgresPoolSizeEnv[] = "A2A_PERF_POSTGRES_POOL_SIZE";
constexpr std::string_view kPerfSchemaPrefix = "a2a_perf_";
constexpr std::string_view kMessageText = "hello";
constexpr std::string_view kPushCallbackUrl = "http://127.0.0.1/fake-push-callback";

constexpr std::string_view kScenarioSendMessageCreateTask = "SendMessage_CreateTask";
constexpr std::string_view kScenarioGetTaskExistingTask = "GetTask_ExistingTask";
constexpr std::string_view kScenarioCancelTaskWorkingTask = "CancelTask_WorkingTask";
constexpr std::string_view kScenarioListTasksNoPagination = "ListTasks_NoPagination";
constexpr std::string_view kScenarioListTasksWithPagination = "ListTasks_WithPagination";
constexpr std::string_view kScenarioSendMessageFollowUpExistingTask = "SendMessage_FollowUpExistingTask";
constexpr std::string_view kScenarioSendMessageFollowUpAtHistoryDepth = "SendMessage_FollowUpAtHistoryDepth/8";
constexpr std::string_view kScenarioGetTaskMissingTaskError = "GetTask_MissingTaskError";
constexpr std::string_view kScenarioSendStreamingMessageFiniteStream = "SendStreamingMessage_FiniteStream";
constexpr std::string_view kScenarioSubscribeToTaskFirstEventLatency = "SubscribeToTask_FirstEventLatency";
constexpr std::string_view kScenarioIdleStreamClientCancellationLatency = "IdleStream_ClientCancellationLatency";
constexpr std::string_view kScenarioSubscribeToTaskMultiSubscriber = "SubscribeToTask_MultiSubscriber";
constexpr std::string_view kScenarioSubscribeToTaskTerminalCompletionLatency =
    "SubscribeToTask_TerminalCompletionLatency";
constexpr std::string_view kScenarioSubscribeToTaskDisconnectOneSubscriber = "SubscribeToTask_DisconnectOneSubscriber";
constexpr std::string_view kScenarioPushConfigCreate = "PushConfig_Create";
constexpr std::string_view kScenarioPushConfigGet = "PushConfig_Get";
constexpr std::string_view kScenarioPushConfigList = "PushConfig_List";
constexpr std::string_view kScenarioPushConfigDelete = "PushConfig_Delete";
constexpr std::string_view kScenarioPushNotifyEndToEndManyConfigs = "PushNotify_EndToEndManyConfigs";
constexpr std::string_view kScenarioPushConfigListManyConfigs = "PushConfig_ListManyConfigs";
constexpr std::string_view kScenarioPushDeliveryCallbackFanout = "PushDelivery_CallbackFanout";
constexpr std::string_view kScenarioPushConfigCreateMany = "PushConfig_CreateMany";
constexpr std::string_view kScenarioPushDeliveryBuildPayload = "PushDelivery_BuildPayload";

constexpr std::array<std::string_view, 22> kScenarios = {
    kScenarioSendMessageCreateTask,
    kScenarioGetTaskExistingTask,
    kScenarioCancelTaskWorkingTask,
    kScenarioListTasksNoPagination,
    kScenarioListTasksWithPagination,
    kScenarioSendMessageFollowUpExistingTask,
    kScenarioSendMessageFollowUpAtHistoryDepth,
    kScenarioGetTaskMissingTaskError,
    kScenarioSendStreamingMessageFiniteStream,
    kScenarioSubscribeToTaskFirstEventLatency,
    kScenarioSubscribeToTaskMultiSubscriber,
    kScenarioSubscribeToTaskTerminalCompletionLatency,
    kScenarioSubscribeToTaskDisconnectOneSubscriber,
    kScenarioPushConfigCreate,
    kScenarioPushConfigGet,
    kScenarioPushConfigList,
    kScenarioPushConfigDelete,
    kScenarioPushNotifyEndToEndManyConfigs,
    kScenarioPushConfigListManyConfigs,
    kScenarioPushDeliveryCallbackFanout,
    kScenarioPushConfigCreateMany,
    kScenarioPushDeliveryBuildPayload,
};

[[nodiscard]] constexpr int ScenarioHistoryDepth(std::string_view scenario) noexcept {
  if (scenario == kScenarioSendMessageFollowUpExistingTask) {
    return kFollowUpHistoryDepth;
  }
  if (scenario == kScenarioSendMessageFollowUpAtHistoryDepth) {
    return kDeepFollowUpHistoryDepth;
  }
  return 0;
}

struct Options final {
  std::string transport = std::string(kGrpcTransport);
  std::string store_backend = std::string(kInMemoryStore);
  int requests = kDefaultRequests;
  int concurrency = kDefaultConcurrency;
  int push_config_fanout = kPushConfigFanout;
  double warmup_seconds = 0.0;
  double duration_seconds = 0.0;
  std::vector<std::string> scenarios;
};

struct ScenarioResult final {
  std::string scenario;
  int operations = 0;
  double measured_duration_seconds = 0.0;
  int success = 0;
  int errors = 0;
  int event_count = 0;
  int successful_deliveries = 0;
  int failed_deliveries = 0;
  int callback_count = 0;
  int fanout_per_operation = 0;
  int total_fanout_count = 0;
  double throughput = 0.0;
  std::vector<double> latencies;
  std::vector<double> first_event_latencies;
  std::vector<double> completion_latencies;
  std::array<std::vector<double>, kPostgresDiagnosticPhaseCount> postgres_phase_latencies;
  std::array<std::size_t, kPostgresDiagnosticPhaseCount> postgres_phase_call_count{};
};

struct OperationOutcome final {
  bool ok = false;
  int event_count = 0;
  double measured_latency_ms = 0.0;
  double first_event_latency_ms = 0.0;
  double completion_latency_ms = 0.0;
  int successful_deliveries = 0;
  int failed_deliveries = 0;
  int callback_count = 0;
  int fanout_per_operation = 0;
  int total_fanout_count = 0;
  std::array<double, kPostgresDiagnosticPhaseCount> postgres_phase_latency_ms{};
  std::array<std::size_t, kPostgresDiagnosticPhaseCount> postgres_phase_call_count{};
};

struct DeleteFixture final {
  std::string task_id;
  std::string config_id;
};

[[nodiscard]] lf::a2a::v1::SendMessageRequest MakeSendRequest(std::string_view message_id,
                                                              std::string_view task_id = {});
[[nodiscard]] lf::a2a::v1::TaskPushNotificationConfig MakePushConfig(std::string_view task_id,
                                                                     std::string_view config_id);
[[nodiscard]] std::string BuildId(std::string_view prefix, int index);
[[nodiscard]] double Percentile(const std::vector<double>& sorted_values, double percentile);
[[nodiscard]] std::vector<std::string> SplitCsv(std::string_view value);
[[nodiscard]] bool HasArgumentValue(int index, int argc);
void SetStringField(google::protobuf::Struct* object, std::string_view key, std::string_view value);
void SetNumberField(google::protobuf::Struct* object, std::string_view key, double value);
void SetIntegerField(google::protobuf::Struct* object, std::string_view key, int value);
void PopulateCommonResultFields(google::protobuf::Struct* object, std::string_view scenario, std::string_view transport,
                                std::string_view store_backend, int concurrency, const ScenarioResult& result);
void AddLatencyField(google::protobuf::Struct* object, const ScenarioResult& result);
void AddPostgresDiagnosticFields(google::protobuf::Struct* object, const ScenarioResult& result);

template <typename ExecuteOperation>
[[nodiscard]] ScenarioResult RunMeasuredScenario(std::string scenario, int requests, int concurrency,
                                                 double duration_seconds, ExecuteOperation execute_operation) {
  struct ThreadResult final {
    int success = 0;
    int errors = 0;
    int event_count = 0;
    int successful_deliveries = 0;
    int failed_deliveries = 0;
    int callback_count = 0;
    int fanout_per_operation = 0;
    int total_fanout_count = 0;
    std::vector<double> latencies;
    std::vector<double> first_event_latencies;
    std::vector<double> completion_latencies;
    std::array<std::vector<double>, kPostgresDiagnosticPhaseCount> postgres_phase_latencies;
    std::array<std::size_t, kPostgresDiagnosticPhaseCount> postgres_phase_call_count{};
  };

  const int worker_count = std::min(concurrency, requests);
  std::atomic<int> next_index{0};
  const bool use_duration_limit = duration_seconds > 0.0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration_seconds);
  std::vector<ThreadResult> thread_results(static_cast<std::size_t>(worker_count));
  const auto started = std::chrono::steady_clock::now();

  {
#if A2A_PERF_HAS_JTHREAD
    using WorkerThread = std::jthread;
#else
    using WorkerThread = std::thread;
#endif
    std::vector<WorkerThread> workers;
    workers.reserve(static_cast<std::size_t>(worker_count));
    for (int worker_index = 0; worker_index < worker_count; ++worker_index) {
      workers.emplace_back([&execute_operation, &next_index, &thread_results, worker_index, requests, concurrency,
                            use_duration_limit, deadline]() {
        auto& thread_result = thread_results[static_cast<std::size_t>(worker_index)];
        const int reserve_count = (requests + concurrency - 1) / concurrency;
        thread_result.latencies.reserve(static_cast<std::size_t>(reserve_count));
        for (;;) {
          if (use_duration_limit && std::chrono::steady_clock::now() >= deadline) {
            return;
          }
          const int operation_index = next_index.fetch_add(1, std::memory_order_relaxed);
          if (operation_index >= requests) {
            return;
          }
          const auto op_started = std::chrono::steady_clock::now();
          const auto outcome = execute_operation(worker_index, operation_index);
          const auto op_finished = std::chrono::steady_clock::now();
          const double latency =
              static_cast<double>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(op_finished - op_started).count()) /
              kNanosecondsPerMillisecond;
          if constexpr (std::is_same_v<std::decay_t<decltype(outcome)>, OperationOutcome>) {
            thread_result.event_count += outcome.event_count;
            thread_result.successful_deliveries += outcome.successful_deliveries;
            thread_result.failed_deliveries += outcome.failed_deliveries;
            thread_result.callback_count += outcome.callback_count;
            thread_result.fanout_per_operation =
                std::max(thread_result.fanout_per_operation, outcome.fanout_per_operation);
            thread_result.total_fanout_count += outcome.total_fanout_count;
            for (std::size_t phase = 0; phase < outcome.postgres_phase_call_count.size(); ++phase) {
              thread_result.postgres_phase_call_count[phase] += outcome.postgres_phase_call_count[phase];
            }
            if (outcome.ok) {
              ++thread_result.success;
              thread_result.latencies.push_back(outcome.measured_latency_ms > 0.0 ? outcome.measured_latency_ms
                                                                                  : latency);
              if (outcome.first_event_latency_ms > 0.0) {
                thread_result.first_event_latencies.push_back(outcome.first_event_latency_ms);
              }
              if (outcome.completion_latency_ms > 0.0) {
                thread_result.completion_latencies.push_back(outcome.completion_latency_ms);
              }
              for (std::size_t phase = 0; phase < outcome.postgres_phase_latency_ms.size(); ++phase) {
                thread_result.postgres_phase_latencies[phase].push_back(outcome.postgres_phase_latency_ms[phase]);
              }
            } else {
              ++thread_result.errors;
            }
          } else if (outcome) {
            ++thread_result.success;
            thread_result.latencies.push_back(latency);
          } else {
            ++thread_result.errors;
          }
        }
      });
    }
#if !A2A_PERF_HAS_JTHREAD
    for (auto& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
#endif
  }

  ScenarioResult result;
  result.scenario = std::move(scenario);
  result.latencies.reserve(static_cast<std::size_t>(requests));
  for (auto& thread_result : thread_results) {
    result.success += thread_result.success;
    result.errors += thread_result.errors;
    result.event_count += thread_result.event_count;
    result.successful_deliveries += thread_result.successful_deliveries;
    result.failed_deliveries += thread_result.failed_deliveries;
    result.callback_count += thread_result.callback_count;
    result.fanout_per_operation = std::max(result.fanout_per_operation, thread_result.fanout_per_operation);
    result.total_fanout_count += thread_result.total_fanout_count;
    result.latencies.insert(result.latencies.end(), std::make_move_iterator(thread_result.latencies.begin()),
                            std::make_move_iterator(thread_result.latencies.end()));
    result.first_event_latencies.insert(result.first_event_latencies.end(),
                                        std::make_move_iterator(thread_result.first_event_latencies.begin()),
                                        std::make_move_iterator(thread_result.first_event_latencies.end()));
    result.completion_latencies.insert(result.completion_latencies.end(),
                                       std::make_move_iterator(thread_result.completion_latencies.begin()),
                                       std::make_move_iterator(thread_result.completion_latencies.end()));
    for (std::size_t phase = 0; phase < result.postgres_phase_latencies.size(); ++phase) {
      auto& destination = result.postgres_phase_latencies[phase];
      auto& source = thread_result.postgres_phase_latencies[phase];
      destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                         std::make_move_iterator(source.end()));
      result.postgres_phase_call_count[phase] += thread_result.postgres_phase_call_count[phase];
    }
  }
  result.operations = result.success + result.errors;
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  result.measured_duration_seconds = elapsed;
  result.throughput = static_cast<double>(result.success) / std::max(elapsed, kMinElapsedSeconds);
  std::ranges::sort(result.latencies);
  std::ranges::sort(result.first_event_latencies);
  std::ranges::sort(result.completion_latencies);
  for (auto& phase_latencies : result.postgres_phase_latencies) {
    std::ranges::sort(phase_latencies);
  }
  return result;
}

}  // namespace a2a::tests::performance
