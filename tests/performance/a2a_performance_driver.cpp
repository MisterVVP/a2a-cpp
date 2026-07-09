// SPDX-License-Identifier: Apache-2.0

#include "a2a_performance_driver.h"

#include <google/protobuf/struct.pb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/core/protojson.h"
#include "a2a/server/push_notification_delivery.h"
#include "a2a/server/request_context.h"
#include "a2a/server/stores/store_factory.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"
#include "example_support/example_support.h"

namespace a2a::tests::performance {

double Percentile(const std::vector<double>& sorted_values, double percentile) {
  if (sorted_values.empty()) {
    return 0.0;
  }
  const auto last = static_cast<double>(sorted_values.size() - 1U);
  const auto index = static_cast<std::size_t>(std::llround((percentile / 100.0) * last));
  return sorted_values[std::min(index, sorted_values.size() - 1U)];
}

lf::a2a::v1::SendMessageRequest MakeSendRequest(std::string_view message_id, std::string_view task_id) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id(std::string(message_id));
  if (!task_id.empty()) {
    request.mutable_message()->set_task_id(std::string(task_id));
  }
  request.mutable_message()->add_parts()->set_text(std::string(kMessageText));
  return request;
}

lf::a2a::v1::TaskPushNotificationConfig MakePushConfig(std::string_view task_id, std::string_view config_id) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(task_id));
  config.set_id(std::string(config_id));
  config.set_url(std::string(kPushCallbackUrl));
  return config;
}

std::string BuildId(std::string_view prefix, int index) {
  std::string value;
  value.reserve(prefix.size() + kIdReserveSlack);
  value.append(prefix);
  value.push_back('-');
  value.append(std::to_string(index));
  return value;
}

}  // namespace a2a::tests::performance

namespace {

using namespace a2a::tests::performance;

class RecordingPushDelivery final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    (void)request;
    deliveries_.fetch_add(1, std::memory_order_relaxed);
    return a2a::server::PushDeliveryResult{.http_status = kHttpStatusOk, .error_message = {}};
  }

 private:
  std::atomic<int> deliveries_{0};
};

class ScenarioHarness final {
 public:
  explicit ScenarioHarness(std::string_view store_backend) {
    if (!ConfigureStores(store_backend)) {
      return;
    }
    options_.push_delivery = &delivery_;
    executor_ = std::make_unique<a2a::examples::ExampleExecutor>(std::move(options_));
    existing_task_id_ = SeedTask("existing-seed");
    subscribe_task_id_ = SeedTask("subscribe-seed");
  }

  [[nodiscard]] bool ok() const noexcept { return executor_ != nullptr; }

  bool Execute(std::string_view scenario, int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    a2a::server::RequestContext context;
    if (auto result = ExecuteTaskScenario(scenario, index, context); result.has_value()) {
      return *result;
    }
    if (auto result = ExecuteStreamingScenario(scenario, index, context); result.has_value()) {
      return *result;
    }
    if (auto result = ExecutePushScenario(scenario, index, context); result.has_value()) {
      return *result;
    }
    return false;
  }

 private:
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
      return SubscribeOnce(context);
    }
    if (scenario == kScenarioSubscribeToTaskMultiSubscriber) {
      return SubscribeMany(kMultiSubscriberCount, context);
    }
    if (scenario == kScenarioSubscribeToTaskTerminalCompletionLatency) {
      return SendAndDrainStream(BuildId("terminal", index), context);
    }
    if (scenario == kScenarioSubscribeToTaskDisconnectOneSubscriber) {
      return SubscribeMany(kDisconnectSubscriberCount, context);
    }
    return std::nullopt;
  }

  std::optional<bool> ExecutePushScenario(std::string_view scenario, int index, a2a::server::RequestContext& context) {
    if (scenario == kScenarioPushConfigCreate) {
      return executor_
          ->CreateTaskPushNotificationConfig(MakePushConfig(existing_task_id_, BuildId("cfg-create", index)), context)
          .ok();
    }
    if (scenario == kScenarioPushConfigGet) {
      return GetPushConfig(context);
    }
    if (scenario == kScenarioPushConfigList) {
      return ListPushConfigs(context);
    }
    if (scenario == kScenarioPushConfigDelete) {
      return DeletePushConfig(BuildId("cfg-delete", index), context);
    }
    if (scenario == kScenarioPushNotifyManyConfigsOneTaskUpdate) {
      return NotifyPushConfigs(index, context);
    }
    if (scenario == kScenarioPushDeliveryCallbackLatency) {
      return NotifyPushConfigs(index, context);
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

  bool SubscribeMany(int subscriber_count, a2a::server::RequestContext& context) {
    for (int subscriber = 0; subscriber < subscriber_count; ++subscriber) {
      if (!SubscribeOnce(context)) {
        return false;
      }
    }
    return true;
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

  bool NotifyPushConfigs(int index, a2a::server::RequestContext& context) {
    for (int config = 0; config < kPushConfigFanout; ++config) {
      SeedPushConfig(existing_task_id_, BuildId("fanout", config));
    }
    return executor_->SendMessage(MakeSendRequest(BuildId("notify", index), existing_task_id_), context).ok();
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
    a2a::server::RequestContext context;
    auto response = executor_->SendMessage(MakeSendRequest(message_id), context);
    if (response.ok() && response.value().has_task()) {
      return response.value().task().id();
    }
    return {};
  }
  void SeedPushConfig(std::string_view task_id, std::string_view config_id) {
    a2a::server::RequestContext context;
    (void)executor_->CreateTaskPushNotificationConfig(MakePushConfig(task_id, config_id), context);
  }
  bool SubscribeOnce(a2a::server::RequestContext& context) {
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(subscribe_task_id_);
    auto stream = executor_->SubscribeTask(request, context);
    return stream.ok() && stream.value()->Next().ok();
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
  std::mutex mutex_;
};

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
    (void)harness.Execute(scenario, warmup_index++);
  }

  struct ThreadResult final {
    int success = 0;
    int errors = 0;
    std::vector<double> latencies;
  };

  const int worker_count = std::min(options.concurrency, options.requests);
  std::atomic<int> next_index{0};
  std::vector<ThreadResult> thread_results(static_cast<std::size_t>(worker_count));
  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(worker_count));

  const auto started = std::chrono::steady_clock::now();
  for (int worker_index = 0; worker_index < worker_count; ++worker_index) {
    workers.emplace_back([&harness, &next_index, &options, &scenario, &thread_results, worker_index]() {
      auto& thread_result = thread_results[static_cast<std::size_t>(worker_index)];
      const int reserve_count = (options.requests + options.concurrency - 1) / options.concurrency;
      thread_result.latencies.reserve(static_cast<std::size_t>(reserve_count));
      for (;;) {
        const int operation_index = next_index.fetch_add(1, std::memory_order_relaxed);
        if (operation_index >= options.requests) {
          return;
        }
        const auto op_started = std::chrono::steady_clock::now();
        const bool ok = harness.Execute(scenario, operation_index);
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
  for (auto& worker : workers) {
    worker.join();
  }

  ScenarioResult result;
  result.scenario = scenario;
  result.operations = options.requests;
  result.latencies.reserve(static_cast<std::size_t>(options.requests));
  for (auto& thread_result : thread_results) {
    result.success += thread_result.success;
    result.errors += thread_result.errors;
    result.latencies.insert(result.latencies.end(), std::make_move_iterator(thread_result.latencies.begin()),
                            std::make_move_iterator(thread_result.latencies.end()));
  }
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  result.throughput = static_cast<double>(result.success) / std::max(elapsed, kMinElapsedSeconds);
  std::ranges::sort(result.latencies);
  return result;
}

bool ParseArgs(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--transport" && index + 1 < argc) {
      options->transport = argv[++index];
    } else if (arg == "--store-backend" && index + 1 < argc) {
      options->store_backend = argv[++index];
    } else if (arg == "--requests" && index + 1 < argc) {
      options->requests = std::atoi(argv[++index]);
    } else if (arg == "--concurrency" && index + 1 < argc) {
      options->concurrency = std::atoi(argv[++index]);
    } else if (arg == "--warmup-seconds" && index + 1 < argc) {
      options->warmup_seconds = std::atof(argv[++index]);
    } else if (arg == "--duration-seconds" && index + 1 < argc) {
      options->duration_seconds = std::atof(argv[++index]);
    } else {
      std::cerr << "unknown or incomplete argument: " << arg << '\n';
      return false;
    }
  }
  return options->requests > 0 && options->concurrency > 0;
}

void SetStringField(google::protobuf::Struct* object, std::string_view key, std::string_view value) {
  (*object->mutable_fields())[std::string(key)].set_string_value(std::string(value));
}

void SetNumberField(google::protobuf::Struct* object, std::string_view key, double value) {
  (*object->mutable_fields())[std::string(key)].set_number_value(value);
}

void SetIntegerField(google::protobuf::Struct* object, std::string_view key, int value) {
  SetNumberField(object, key, static_cast<double>(value));
}

google::protobuf::Struct BuildResultObject(const Options& options, const ScenarioResult& result) {
  google::protobuf::Struct object;
  SetStringField(&object, "scenario", result.scenario);
  SetStringField(&object, "transport", options.transport);
  SetStringField(&object, "store_backend", options.store_backend);
  SetIntegerField(&object, "concurrency", options.concurrency);
  SetIntegerField(&object, "operations", result.operations);
  SetIntegerField(&object, "success", result.success);
  SetIntegerField(&object, "errors", result.errors);
  SetNumberField(&object, "throughput_ops_per_sec", result.throughput);
  SetNumberField(&object, "warmup_seconds", options.warmup_seconds);
  SetNumberField(&object, "duration_seconds", options.duration_seconds);
  SetStringField(&object, "driver_type", kDriverType);

  std::string transport_path;
  transport_path.reserve(kSdkTransportPathPrefix.size() + options.transport.size() + kServerDispatchSuffix.size());
  transport_path.append(kSdkTransportPathPrefix);
  transport_path.append(options.transport);
  transport_path.append(kServerDispatchSuffix);
  SetStringField(&object, "transport_path", transport_path);

  google::protobuf::Struct latency;
  SetNumberField(&latency, "p50", Percentile(result.latencies, kP50));
  SetNumberField(&latency, "p90", Percentile(result.latencies, kP90));
  SetNumberField(&latency, "p95", Percentile(result.latencies, kP95));
  SetNumberField(&latency, "p99", Percentile(result.latencies, kP99));
  SetNumberField(&latency, "max", result.latencies.empty() ? 0.0 : result.latencies.back());
  (*object.mutable_fields())["latency_ms"].mutable_struct_value()->Swap(&latency);
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
}  // namespace

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
  for (const std::string_view scenario : kScenarios) {
    WriteResultJson(options, RunScenario(options, std::string(scenario)), first);
    first = false;
  }
  std::cout << "\n]\n";
  return 0;
}
