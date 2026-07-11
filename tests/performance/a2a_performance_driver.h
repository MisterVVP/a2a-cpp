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
constexpr int kMultiSubscriberCount = 3;
constexpr int kDisconnectSubscriberCount = 2;
constexpr int kHttpStatusOk = 200;
constexpr int kUsageExitCode = 2;
constexpr double kP50 = 50.0;
constexpr double kP90 = 90.0;
constexpr double kP95 = 95.0;
constexpr double kP99 = 99.0;
constexpr std::size_t kIdReserveSlack = 16U;
constexpr char kPostgresDsnEnv[] = "A2A_TEST_POSTGRES_DSN";
constexpr std::string_view kPerfSchemaPrefix = "a2a_perf_";
constexpr std::string_view kMessageText = "hello";
constexpr std::string_view kPushCallbackUrl = "http://127.0.0.1/fake-push-callback";

constexpr std::string_view kScenarioSendMessageCreateTask = "SendMessage_CreateTask";
constexpr std::string_view kScenarioGetTaskExistingTask = "GetTask_ExistingTask";
constexpr std::string_view kScenarioCancelTaskWorkingTask = "CancelTask_WorkingTask";
constexpr std::string_view kScenarioListTasksNoPagination = "ListTasks_NoPagination";
constexpr std::string_view kScenarioListTasksWithPagination = "ListTasks_WithPagination";
constexpr std::string_view kScenarioSendMessageFollowUpExistingTask = "SendMessage_FollowUpExistingTask";
constexpr std::string_view kScenarioGetTaskMissingTaskError = "GetTask_MissingTaskError";
constexpr std::string_view kScenarioSendStreamingMessageFiniteStream = "SendStreamingMessage_FiniteStream";
constexpr std::string_view kScenarioSubscribeToTaskFirstEventLatency = "SubscribeToTask_FirstEventLatency";
constexpr std::string_view kScenarioSubscribeToTaskMultiSubscriber = "SubscribeToTask_MultiSubscriber";
constexpr std::string_view kScenarioSubscribeToTaskTerminalCompletionLatency =
    "SubscribeToTask_TerminalCompletionLatency";
constexpr std::string_view kScenarioSubscribeToTaskDisconnectOneSubscriber = "SubscribeToTask_DisconnectOneSubscriber";
constexpr std::string_view kScenarioPushConfigCreate = "PushConfig_Create";
constexpr std::string_view kScenarioPushConfigGet = "PushConfig_Get";
constexpr std::string_view kScenarioPushConfigList = "PushConfig_List";
constexpr std::string_view kScenarioPushConfigDelete = "PushConfig_Delete";
constexpr std::string_view kScenarioPushNotifyManyConfigsOneTaskUpdate = "PushNotify_ManyConfigsOneTaskUpdate";
constexpr std::string_view kScenarioPushDeliveryCallbackLatency = "PushDelivery_CallbackLatency";

constexpr std::array<std::string_view, 18> kScenarios = {
    kScenarioSendMessageCreateTask,
    kScenarioGetTaskExistingTask,
    kScenarioCancelTaskWorkingTask,
    kScenarioListTasksNoPagination,
    kScenarioListTasksWithPagination,
    kScenarioSendMessageFollowUpExistingTask,
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
    kScenarioPushNotifyManyConfigsOneTaskUpdate,
    kScenarioPushDeliveryCallbackLatency,
};

struct Options final {
  std::string transport = std::string(kGrpcTransport);
  std::string store_backend = std::string(kInMemoryStore);
  int requests = kDefaultRequests;
  int concurrency = kDefaultConcurrency;
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
  double throughput = 0.0;
  std::vector<double> latencies;
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

template <typename ExecuteOperation>
[[nodiscard]] ScenarioResult RunMeasuredScenario(std::string scenario, int requests, int concurrency,
                                                 double duration_seconds, ExecuteOperation execute_operation) {
  struct ThreadResult final {
    int success = 0;
    int errors = 0;
    std::vector<double> latencies;
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
          const bool ok = execute_operation(worker_index, operation_index);
          const auto op_finished = std::chrono::steady_clock::now();
          const double latency =
              static_cast<double>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(op_finished - op_started).count()) /
              kNanosecondsPerMillisecond;
          if (ok) {
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
    result.latencies.insert(result.latencies.end(), std::make_move_iterator(thread_result.latencies.begin()),
                            std::make_move_iterator(thread_result.latencies.end()));
  }
  result.operations = result.success + result.errors;
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  result.measured_duration_seconds = elapsed;
  result.throughput = static_cast<double>(result.success) / std::max(elapsed, kMinElapsedSeconds);
  std::ranges::sort(result.latencies);
  return result;
}

}  // namespace a2a::tests::performance
