// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

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
constexpr std::string_view kSdkTransportPathPrefix = "sdk_";
constexpr std::string_view kServerDispatchSuffix = "_server_dispatch";
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
};

struct ScenarioResult final {
  std::string scenario;
  int operations = 0;
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

}  // namespace a2a::tests::performance
