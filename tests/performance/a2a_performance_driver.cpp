// SPDX-License-Identifier: Apache-2.0

#include "a2a_performance_driver.h"

#include <google/protobuf/struct.pb.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/protojson.h"
#include "a2a/server/push_notification_delivery.h"
#include "a2a/server/push_notification_service.h"
#include "a2a/server/request_context.h"
#include "a2a/server/server_stream_session.h"
#include "a2a/server/stores/store_factory.h"
#ifdef A2A_ENABLE_POSTGRES_STORE
#include "a2a/server/stores/postgres_common.h"
#endif
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"
#include "example_support/example_support.h"

namespace {

using namespace a2a::tests::performance;

constexpr std::chrono::milliseconds kStreamWaitTimeout{5000};
constexpr int kNoFailingDelivery = std::numeric_limits<int>::max();
constexpr std::string_view kRecordingDeliveryFailure = "recording push delivery failure";

struct DeliveryStats final {
  int attempted = 0;
  int succeeded = 0;
  int failed = 0;
};

struct ScenarioInstrumentation final {
  std::atomic<int> task_creates{0};
  std::atomic<int> config_creates{0};
  std::atomic<int> config_lists{0};
  std::atomic<int> payload_builds{0};
};

class RecordingPushDelivery final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    const std::string& task_id = request.payload.status_update().task_id();
    bool should_fail = false;
    {
      std::lock_guard lock(stats_mutex_);
      DeliveryStats& stats = stats_by_task_[task_id];
      ++stats.attempted;
      should_fail = stats.attempted == failing_delivery_index_;
      if (should_fail) {
        ++stats.failed;
      } else {
        ++stats.succeeded;
      }
    }
    if (should_fail) {
      return a2a::core::Error::Network(std::string(kRecordingDeliveryFailure));
    }
    return a2a::server::PushDeliveryResult{.http_status = kHttpStatusOk, .error_message = {}};
  }

  void set_failing_delivery_index(int failing_delivery_index) {
    std::lock_guard lock(stats_mutex_);
    failing_delivery_index_ = failing_delivery_index;
  }

  [[nodiscard]] DeliveryStats TakeStats(std::string_view task_id) {
    std::lock_guard lock(stats_mutex_);
    const auto stats = stats_by_task_.find(std::string(task_id));
    if (stats == stats_by_task_.end()) {
      return {};
    }
    const DeliveryStats result = stats->second;
    stats_by_task_.erase(stats);
    return result;
  }

 private:
  std::mutex stats_mutex_;
  std::unordered_map<std::string, DeliveryStats> stats_by_task_;
  int failing_delivery_index_ = kNoFailingDelivery;
};

lf::a2a::v1::Task BuildRepresentativePushPayloadTask(std::string_view task_id) {
  lf::a2a::v1::Task task;
  task.set_id(std::string(task_id));
  task.set_context_id(BuildId("payload-context", 0));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  return task;
}

lf::a2a::v1::StreamResponse BuildRepresentativePushPayload(std::string_view task_id) {
  return a2a::server::BuildTaskStatusUpdatePayload(BuildRepresentativePushPayloadTask(task_id));
}

class ScenarioHarness final {
 public:
  explicit ScenarioHarness(std::string_view store_backend, ScenarioInstrumentation* instrumentation = nullptr)
      : instrumentation_(instrumentation) {
    if (!ConfigureStores(store_backend)) {
      return;
    }
    options_.push_delivery = &delivery_;
    executor_ = std::make_unique<a2a::examples::ExampleExecutor>(std::move(options_));
    existing_task_id_ = SeedTask("existing-seed");
    subscribe_task_id_ = SeedTask("subscribe-seed");
    fanout_task_id_ = SeedTask("fanout-seed");
    create_many_task_id_ = SeedTask("create-many-seed");
    SeedFanoutConfigs(fanout_task_id_, "preloaded-fanout");
    prebuilt_payload_ = BuildPushPayload(fanout_task_id_);
  }

  [[nodiscard]] bool ok() const noexcept { return executor_ != nullptr; }

  void set_failing_delivery_index(int failing_delivery_index) noexcept {
    delivery_.set_failing_delivery_index(failing_delivery_index);
  }

  OperationOutcome Execute(std::string_view scenario, int worker_index, int index) {
#ifdef A2A_ENABLE_POSTGRES_STORE
    a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
#endif
    a2a::server::RequestContext context;
    context.client_headers.emplace("perf.worker", BuildId("worker", worker_index));
    if (auto result = ExecuteTaskScenario(scenario, index, context); result.has_value()) {
      return FinishOperation(OperationSucceeded(*result));
    }
    if (auto result = ExecuteStreamingScenario(scenario, index, context); result.has_value()) {
      return FinishOperation(OperationSucceeded(*result));
    }
    if (auto result = ExecutePushScenario(scenario, index, context); result.has_value()) {
      return FinishOperation(*result);
    }
    return {};
  }

 private:
  static OperationOutcome OperationSucceeded(bool ok) { return {.ok = ok, .event_count = ok ? 1 : 0}; }

  static OperationOutcome FinishOperation(OperationOutcome outcome) {
#ifdef A2A_ENABLE_POSTGRES_STORE
    const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
    outcome.postgres_phase_latency_ms = diagnostics.elapsed_ms;
#endif
    return outcome;
  }

  bool ConfigureStores(std::string_view store_backend) {
    if (store_backend == kInMemoryStore) {
      return true;
    }
    if (store_backend != kPostgresStore) {
      std::cerr << "unsupported store backend: " << store_backend << '\n';
      return false;
    }
    const char* dsn = std::getenv(kPostgresDsnEnv);
    if (dsn == nullptr || std::string_view(dsn).empty()) {
      std::cerr << kPostgresDsnEnv << " must be set for postgres performance scenarios\n";
      return false;
    }
    a2a::server::stores::PostgresStoreFactory factory({.connection_string = dsn, .schema = MakePostgresSchema()});
    auto bundle = factory.CreateStoreBundle();
    if (!bundle.ok()) {
      std::cerr << "failed to create postgres performance stores: " << bundle.error().message() << '\n';
      return false;
    }
    store_bundle_ = std::move(bundle.value());
    options_.task_store = store_bundle_.task_store.get();
    options_.push_store = store_bundle_.push_store.get();
    return true;
  }

  std::optional<bool> ExecuteTaskScenario(std::string_view scenario, int index, a2a::server::RequestContext& context) {
    if (scenario == kScenarioSendMessageCreateTask) {
      return executor_->SendMessage(MakeSendRequest(BuildId("create", index)), context).ok();
    }
    if (scenario == kScenarioGetTaskExistingTask) {
      lf::a2a::v1::GetTaskRequest request;
      request.set_id(existing_task_id_);
      return executor_->GetTask(request, context).ok();
    }
    if (scenario == kScenarioCancelTaskWorkingTask) {
      const std::string task_id = SeedTask(BuildId("cancel", index));
      lf::a2a::v1::CancelTaskRequest request;
      request.set_id(task_id);
      return executor_->CancelTask(request, context).ok();
    }
    if (scenario == kScenarioListTasksNoPagination) {
      return ListTasks(/*use_pagination=*/false, context);
    }
    if (scenario == kScenarioListTasksWithPagination) {
      return ListTasks(/*use_pagination=*/true, context);
    }
    if (scenario == kScenarioSendMessageFollowUpExistingTask) {
      return executor_->SendMessage(MakeSendRequest(BuildId("follow", index), existing_task_id_), context).ok();
    }
    if (scenario == kScenarioGetTaskMissingTaskError) {
      lf::a2a::v1::GetTaskRequest request;
      request.set_id(BuildId("missing", index));
      return !executor_->GetTask(request, context).ok();
    }
    return std::nullopt;
  }

  std::optional<bool> ExecuteStreamingScenario(std::string_view scenario, int index,
                                               a2a::server::RequestContext& context) {
    if (scenario == kScenarioSendStreamingMessageFiniteStream) {
      return SendAndDrainStream(BuildId("stream", index), context);
    }
    if (scenario == kScenarioSubscribeToTaskFirstEventLatency) {
      return SubscribeOnce(BuildId("subscribe-first", index), context);
    }
    if (scenario == kScenarioSubscribeToTaskMultiSubscriber) {
      return SubscribeMany(kMultiSubscriberCount, BuildId("subscribe-many", index), context);
    }
    if (scenario == kScenarioSubscribeToTaskTerminalCompletionLatency) {
      return SubscribeTerminalCompletion(BuildId("subscribe-terminal", index), context);
    }
    if (scenario == kScenarioSubscribeToTaskDisconnectOneSubscriber) {
      return SubscribeDisconnectOne(BuildId("subscribe-disconnect", index), context);
    }
    return std::nullopt;
  }

  std::optional<OperationOutcome> ExecutePushScenario(std::string_view scenario, int index,
                                                      a2a::server::RequestContext& context) {
    if (scenario == kScenarioPushConfigCreate) {
      return OperationSucceeded(executor_
                                    ->CreateTaskPushNotificationConfig(
                                        MakePushConfig(existing_task_id_, BuildId("cfg-create", index)), context)
                                    .ok());
    }
    if (scenario == kScenarioPushConfigGet) {
      return OperationSucceeded(GetPushConfig(context));
    }
    if (scenario == kScenarioPushConfigList) {
      return OperationSucceeded(ListPushConfigs(context));
    }
    if (scenario == kScenarioPushConfigDelete) {
      return OperationSucceeded(DeletePushConfig(BuildId("cfg-delete", index), context));
    }
    if (scenario == kScenarioPushNotifyEndToEndManyConfigs) {
      return NotifyPushConfigs(index, context);
    }
    if (scenario == kScenarioPushConfigListManyConfigs) {
      return ListManyPushConfigs(context);
    }
    if (scenario == kScenarioPushDeliveryCallbackFanout) {
      return DeliverPreloadedCallbacks();
    }
    if (scenario == kScenarioPushConfigCreateMany) {
      return CreateManyPushConfigs(index, context);
    }
    if (scenario == kScenarioPushDeliveryBuildPayload) {
      return BuildPushPayloadOnly();
    }
    return std::nullopt;
  }

  bool ListTasks(bool use_pagination, a2a::server::RequestContext& context) {
    a2a::server::ListTasksRequest request;
    if (use_pagination) {
      request.page_size = kListPageSize;
    }
    return executor_->ListTasks(request, context).ok();
  }

  bool SendAndDrainStream(std::string_view message_id, a2a::server::RequestContext& context) {
    auto stream = executor_->SendStreamingMessage(MakeSendRequest(message_id), context);
    return stream.ok() && DrainStream(stream.value().get());
  }

  bool SubscribeMany(int subscriber_count, std::string_view message_id, a2a::server::RequestContext& context) {
    const std::string task_id = SeedTask(message_id);
    if (task_id.empty()) {
      return false;
    }
    std::vector<std::unique_ptr<a2a::server::ServerStreamSession>> streams;
    if (!OpenSubscriptions(task_id, subscriber_count, context, &streams)) {
      return false;
    }
    auto waiters = WaitForNextEvents(&streams);
    const bool published =
        executor_->SendMessage(MakeSendRequest(BuildId("subscribe-update", subscriber_count), task_id), context).ok();
    return published && AllWaitersDelivered(&waiters);
  }

  bool SubscribeDisconnectOne(std::string_view message_id, a2a::server::RequestContext& context) {
    const std::string task_id = SeedTask(message_id);
    if (task_id.empty()) {
      return false;
    }
    std::vector<std::unique_ptr<a2a::server::ServerStreamSession>> streams;
    if (!OpenSubscriptions(task_id, kDisconnectSubscriberCount, context, &streams)) {
      return false;
    }
    streams.front()->Cancel();
    streams.erase(streams.begin());
    auto waiters = WaitForNextEvents(&streams);
    const bool published = executor_->SendMessage(MakeSendRequest("disconnect-update", task_id), context).ok();
    return published && AllWaitersDelivered(&waiters);
  }

  bool SubscribeTerminalCompletion(std::string_view message_id, a2a::server::RequestContext& context) {
    const std::string task_id = SeedTask(message_id);
    if (task_id.empty()) {
      return false;
    }
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(task_id);
    auto stream = executor_->SubscribeTask(request, context);
    if (!stream.ok() || !ExpectNextEvent(stream.value().get())) {
      return false;
    }
    lf::a2a::v1::CancelTaskRequest cancel_request;
    cancel_request.set_id(task_id);
    if (!executor_->CancelTask(cancel_request, context).ok()) {
      return false;
    }
    auto terminal = stream.value()->NextFor(kStreamWaitTimeout);
    if (!terminal.ok()) {
      return false;
    }
    const auto& terminal_event = terminal.value();
    if (!terminal_event.has_value() || !terminal_event->has_status_update()) {
      return false;
    }
    auto completion = stream.value()->NextFor(kStreamWaitTimeout);
    return completion.ok() && !completion.value().has_value();
  }

  bool GetPushConfig(a2a::server::RequestContext& context) {
    SeedPushConfig(existing_task_id_, "cfg-get");
    lf::a2a::v1::GetTaskPushNotificationConfigRequest request;
    request.set_task_id(existing_task_id_);
    request.set_id("cfg-get");
    return executor_->GetTaskPushNotificationConfig(request, context).ok();
  }

  bool ListPushConfigs(a2a::server::RequestContext& context) {
    SeedPushConfig(existing_task_id_, "cfg-list");
    lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
    request.set_task_id(existing_task_id_);
    return executor_->ListTaskPushNotificationConfigs(request, context).ok();
  }

  bool DeletePushConfig(std::string_view config_id, a2a::server::RequestContext& context) {
    SeedPushConfig(existing_task_id_, config_id);
    lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request;
    request.set_task_id(existing_task_id_);
    request.set_id(std::string(config_id));
    return executor_->DeleteTaskPushNotificationConfig(request, context).ok();
  }

  OperationOutcome NotifyPushConfigs(int index, a2a::server::RequestContext& context) {
    const std::string task_id = SeedTask(BuildId("notify-task", index));
    if (task_id.empty()) {
      return {};
    }
    SeedFanoutConfigs(task_id, BuildId("fanout", index));
    const bool ok = executor_->SendMessage(MakeSendRequest(BuildId("notify", index), task_id), context).ok();
    return OutcomeFromDeliveryStats(ok, delivery_.TakeStats(task_id), kPushConfigFanout);
  }

  OperationOutcome ListManyPushConfigs(a2a::server::RequestContext& context) {
    lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
    request.set_task_id(fanout_task_id_);
    CountListConfig();
    const auto configs = executor_->ListTaskPushNotificationConfigs(request, context);
    const bool ok = configs.ok() && configs.value().configs_size() == kPushConfigFanout;
    return {.ok = ok,
            .event_count = ok ? configs.value().configs_size() : 0,
            .fanout_per_operation = kPushConfigFanout,
            .total_fanout_count = kPushConfigFanout};
  }

  OperationOutcome DeliverPreloadedCallbacks() {
    int successful_deliveries = 0;
    int failed_deliveries = 0;
    for (const auto& config : preloaded_configs_) {
      const auto delivered = delivery_.Deliver({.config = config, .payload = prebuilt_payload_});
      if (delivered.ok()) {
        ++successful_deliveries;
      } else {
        ++failed_deliveries;
      }
    }
    return {.ok = failed_deliveries == 0,
            .successful_deliveries = successful_deliveries,
            .failed_deliveries = failed_deliveries,
            .callback_count = successful_deliveries + failed_deliveries,
            .fanout_per_operation = static_cast<int>(preloaded_configs_.size()),
            .total_fanout_count = successful_deliveries + failed_deliveries};
  }

  OperationOutcome CreateManyPushConfigs(int index, a2a::server::RequestContext& context) {
    if (create_many_task_id_.empty()) {
      return {};
    }
    int created = 0;
    for (int config = 0; config < kPushConfigFanout; ++config) {
      const auto response = executor_->CreateTaskPushNotificationConfig(
          MakePushConfig(create_many_task_id_, BuildId(BuildId("create-many", index), config)), context);
      CountConfigCreate();
      if (!response.ok()) {
        return {
            .event_count = created, .fanout_per_operation = kPushConfigFanout, .total_fanout_count = kPushConfigFanout};
      }
      ++created;
    }
    return {.ok = true,
            .event_count = created,
            .fanout_per_operation = kPushConfigFanout,
            .total_fanout_count = kPushConfigFanout};
  }

  OperationOutcome BuildPushPayloadOnly() {
    const auto payload = BuildPushPayload(fanout_task_id_);
    return {.ok = payload.has_status_update(), .event_count = payload.has_status_update() ? 1 : 0};
  }

  static std::string MakePostgresSchema() {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string schema;
    schema.reserve(kPerfSchemaPrefix.size() + kIdReserveSlack);
    schema.append(kPerfSchemaPrefix);
    schema.append(std::to_string(ticks));
    return schema;
  }

  std::string SeedTask(std::string_view message_id) {
    CountTaskCreate();
    a2a::server::RequestContext context;
    auto response = executor_->SendMessage(MakeSendRequest(message_id), context);
    if (response.ok() && response.value().has_task()) {
      return response.value().task().id();
    }
    return {};
  }
  void SeedPushConfig(std::string_view task_id, std::string_view config_id) {
    CountConfigCreate();
    a2a::server::RequestContext context;
    (void)executor_->CreateTaskPushNotificationConfig(MakePushConfig(task_id, config_id), context);
  }

  void SeedFanoutConfigs(std::string_view task_id, std::string_view config_prefix) {
    const bool capture_preloaded = task_id == fanout_task_id_;
    if (capture_preloaded) {
      preloaded_configs_.clear();
      preloaded_configs_.reserve(kPushConfigFanout);
    }
    for (int config = 0; config < kPushConfigFanout; ++config) {
      lf::a2a::v1::TaskPushNotificationConfig push_config = MakePushConfig(task_id, BuildId(config_prefix, config));
      SeedPushConfig(task_id, push_config.id());
      if (capture_preloaded) {
        preloaded_configs_.push_back(std::move(push_config));
      }
    }
  }

  lf::a2a::v1::StreamResponse BuildPushPayload(std::string_view task_id) {
    CountPayloadBuild();
    return BuildRepresentativePushPayload(task_id);
  }

  static OperationOutcome OutcomeFromDeliveryStats(bool ok, const DeliveryStats& stats,
                                                   int configured_fanout) noexcept {
    return {.ok = ok,
            .successful_deliveries = stats.succeeded,
            .failed_deliveries = stats.failed,
            .callback_count = stats.attempted,
            .fanout_per_operation = configured_fanout,
            .total_fanout_count = stats.attempted};
  }

  void CountTaskCreate() const {
    if (instrumentation_ != nullptr) {
      instrumentation_->task_creates.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void CountConfigCreate() const {
    if (instrumentation_ != nullptr) {
      instrumentation_->config_creates.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void CountListConfig() const {
    if (instrumentation_ != nullptr) {
      instrumentation_->config_lists.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void CountPayloadBuild() const {
    if (instrumentation_ != nullptr) {
      instrumentation_->payload_builds.fetch_add(1, std::memory_order_relaxed);
    }
  }
  bool SubscribeOnce(std::string_view message_id, a2a::server::RequestContext& context) {
    const std::string task_id = SeedTask(message_id);
    if (task_id.empty()) {
      return false;
    }
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(task_id);
    auto stream = executor_->SubscribeTask(request, context);
    return stream.ok() && ExpectNextEvent(stream.value().get());
  }

  static bool ExpectNextEvent(a2a::server::ServerStreamSession* stream) {
    auto event = stream->NextFor(kStreamWaitTimeout);
    return event.ok() && event.value().has_value();
  }

  bool OpenSubscriptions(std::string_view task_id, int subscriber_count, a2a::server::RequestContext& context,
                         std::vector<std::unique_ptr<a2a::server::ServerStreamSession>>* streams) {
    streams->reserve(static_cast<std::size_t>(subscriber_count));
    for (int subscriber = 0; subscriber < subscriber_count; ++subscriber) {
      lf::a2a::v1::GetTaskRequest request;
      request.set_id(std::string(task_id));
      auto stream = executor_->SubscribeTask(request, context);
      if (!stream.ok() || !ExpectNextEvent(stream.value().get())) {
        return false;
      }
      streams->push_back(std::move(stream.value()));
    }
    return true;
  }

  static std::vector<std::future<bool>> WaitForNextEvents(
      std::vector<std::unique_ptr<a2a::server::ServerStreamSession>>* streams) {
    std::vector<std::future<bool>> waiters;
    waiters.reserve(streams->size());
    for (auto& stream : *streams) {
      waiters.push_back(std::async(std::launch::async, [&stream]() { return ExpectNextEvent(stream.get()); }));
    }
    return waiters;
  }

  static bool AllWaitersDelivered(std::vector<std::future<bool>>* waiters) {
    return std::ranges::all_of(*waiters, [](std::future<bool>& waiter) { return waiter.get(); });
  }
  static bool DrainStream(a2a::server::ServerStreamSession* stream) {
    for (;;) {
      auto next = stream->Next();
      if (!next.ok()) {
        return false;
      }
      if (!next.value().has_value()) {
        return true;
      }
    }
  }

  RecordingPushDelivery delivery_;
  a2a::server::stores::StoreBundle store_bundle_;
  a2a::examples::ExampleExecutorOptions options_;
  std::unique_ptr<a2a::examples::ExampleExecutor> executor_;
  std::string existing_task_id_;
  std::string subscribe_task_id_;
  std::string fanout_task_id_;
  std::string create_many_task_id_;
  std::vector<lf::a2a::v1::TaskPushNotificationConfig> preloaded_configs_;
  lf::a2a::v1::StreamResponse prebuilt_payload_;
  ScenarioInstrumentation* instrumentation_ = nullptr;
};

}  // namespace

#ifndef A2A_PERFORMANCE_DRIVER_DISABLE_MAIN
ScenarioResult RunScenario(const Options& options, const std::string& scenario) {
  ScenarioHarness harness(options.store_backend);
  if (!harness.ok()) {
    ScenarioResult failed;
    failed.scenario = scenario;
    failed.operations = options.requests;
    failed.errors = options.requests;
    return failed;
  }
  const auto warmup_end = std::chrono::steady_clock::now() + std::chrono::duration<double>(options.warmup_seconds);
  int warmup_index = 0;
  while (std::chrono::steady_clock::now() < warmup_end) {
    (void)harness.Execute(scenario, 0, warmup_index++);
  }

  return RunMeasuredScenario(
      scenario, options.requests, options.concurrency, options.duration_seconds,
      [&harness, &scenario](int worker_index, int index) { return harness.Execute(scenario, worker_index, index); });
}

bool IsSupportedScenario(std::string_view scenario) {
  return std::ranges::find(kScenarios, scenario) != kScenarios.end();
}

std::vector<std::string> SelectedScenarios(const Options& options) {
  if (!options.scenarios.empty()) {
    return options.scenarios;
  }
  std::vector<std::string> scenarios;
  scenarios.reserve(kScenarios.size());
  for (const std::string_view scenario : kScenarios) {
    scenarios.emplace_back(scenario);
  }
  return scenarios;
}

bool ParseScenarioSelection(std::string_view value, Options* options) {
  options->scenarios = SplitCsv(value);
  if (options->scenarios.empty()) {
    std::cerr << "scenario selection must not be empty\n";
    return false;
  }
  for (const std::string& scenario : options->scenarios) {
    if (!IsSupportedScenario(scenario)) {
      std::cerr << "unsupported scenario: " << scenario << '\n';
      return false;
    }
  }
  return true;
}

bool ParseArgs(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (!HasArgumentValue(index, argc)) {
      std::cerr << "unknown or incomplete argument: " << arg << '\n';
      return false;
    }
    const char* raw_value = argv[++index];
    const std::string_view value(raw_value);
    if (arg == "--transport") {
      options->transport = value;
    } else if (arg == "--store-backend") {
      options->store_backend = value;
    } else if (arg == "--requests") {
      options->requests = std::atoi(raw_value);
    } else if (arg == "--concurrency") {
      options->concurrency = std::atoi(raw_value);
    } else if (arg == "--warmup-seconds") {
      options->warmup_seconds = std::atof(raw_value);
    } else if (arg == "--duration-seconds") {
      options->duration_seconds = std::atof(raw_value);
    } else if (arg == "--scenarios") {
      if (!ParseScenarioSelection(value, options)) {
        return false;
      }
    } else {
      std::cerr << "unknown or incomplete argument: " << arg << '\n';
      return false;
    }
  }
  return options->requests > 0 && options->concurrency > 0;
}

google::protobuf::Struct BuildResultObject(const Options& options, const ScenarioResult& result) {
  google::protobuf::Struct object;
  PopulateCommonResultFields(&object, result.scenario, options.transport, options.store_backend, options.concurrency,
                             result);
  SetNumberField(&object, "warmup_seconds", options.warmup_seconds);
  SetIntegerField(&object, "configured_requests", options.requests);
  SetNumberField(&object, "configured_duration_seconds", options.duration_seconds);
  SetNumberField(&object, "measured_duration_seconds", result.measured_duration_seconds);
  SetNumberField(&object, "duration_seconds", options.duration_seconds);
  SetStringField(&object, "driver_type", kDriverType);

  SetStringField(&object, "transport_path", "in_process");

  AddLatencyField(&object, result);
  if (options.store_backend == kPostgresStore) {
    AddPostgresDiagnosticFields(&object, result);
  }
  return object;
}

void WriteResultJson(const Options& options, const ScenarioResult& result, bool first) {
  if (!first) {
    std::cout << ",\n";
  }
  const auto json = a2a::core::MessageToJson(BuildResultObject(options, result));
  if (!json.ok()) {
    std::cout << "{}";
    return;
  }
  std::cout << "  " << json.value();
}

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    return kUsageExitCode;
  }
  if ((options.store_backend != kInMemoryStore && options.store_backend != kPostgresStore) ||
      (options.transport != kGrpcTransport && options.transport != kJsonRpcTransport &&
       options.transport != kHttpJsonTransport)) {
    std::cerr << "unsupported transport or store backend\n";
    return kUsageExitCode;
  }
  std::cout << "[\n";
  bool first = true;
  for (const std::string& scenario : SelectedScenarios(options)) {
    WriteResultJson(options, RunScenario(options, scenario), first);
    first = false;
  }
  std::cout << "\n]\n";
  return 0;
}

#endif  // A2A_PERFORMANCE_DRIVER_DISABLE_MAIN
