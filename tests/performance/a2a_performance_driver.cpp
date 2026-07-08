// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "a2a/server/push_notification_delivery.h"
#include "a2a/server/request_context.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"
#include "example_support/example_support.h"

namespace {
constexpr std::string_view kGrpcTransport = "grpc";
constexpr std::string_view kJsonRpcTransport = "jsonrpc";
constexpr std::string_view kHttpJsonTransport = "http_json";
constexpr std::string_view kInMemoryStore = "inmemory";
constexpr std::string_view kPostgresStore = "postgres";
constexpr std::string_view kDriverType = "cpp_sdk_in_process";
constexpr double kNanosecondsPerMillisecond = 1000000.0;
constexpr int kDefaultRequests = 1000;
constexpr int kDefaultConcurrency = 1;
constexpr int kListPageSize = 10;
constexpr int kPushConfigFanout = 8;

const std::vector<std::string>& Scenarios() {
  static const std::vector<std::string> scenarios = {"SendMessage_CreateTask",
                                                     "GetTask_ExistingTask",
                                                     "CancelTask_WorkingTask",
                                                     "ListTasks_NoPagination",
                                                     "ListTasks_WithPagination",
                                                     "SendMessage_FollowUpExistingTask",
                                                     "GetTask_MissingTaskError",
                                                     "SendStreamingMessage_FiniteStream",
                                                     "SubscribeToTask_FirstEventLatency",
                                                     "SubscribeToTask_MultiSubscriber",
                                                     "SubscribeToTask_TerminalCompletionLatency",
                                                     "SubscribeToTask_DisconnectOneSubscriber",
                                                     "PushConfig_Create",
                                                     "PushConfig_Get",
                                                     "PushConfig_List",
                                                     "PushConfig_Delete",
                                                     "PushNotify_ManyConfigsOneTaskUpdate",
                                                     "PushDelivery_CallbackLatency"};
  return scenarios;
}

class RecordingPushDelivery final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    (void)request;
    std::lock_guard<std::mutex> lock(mutex_);
    ++deliveries_;
    return a2a::server::PushDeliveryResult{.http_status = 200, .error_message = {}};
  }

 private:
  std::mutex mutex_;
  int deliveries_ = 0;
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

std::string JsonEscape(std::string_view value) {
  std::ostringstream escaped;
  for (const char character : value) {
    switch (character) {
      case '"':
        escaped << "\\\"";
        break;
      case '\\':
        escaped << "\\\\";
        break;
      case '\n':
        escaped << "\\n";
        break;
      default:
        escaped << character;
        break;
    }
  }
  return escaped.str();
}

double Percentile(const std::vector<double>& sorted_values, double percentile) {
  if (sorted_values.empty()) return 0.0;
  const auto last = static_cast<double>(sorted_values.size() - 1U);
  const auto index = static_cast<std::size_t>(std::llround((percentile / 100.0) * last));
  return sorted_values[std::min(index, sorted_values.size() - 1U)];
}

lf::a2a::v1::SendMessageRequest MakeSendRequest(std::string_view message_id, std::string_view task_id = {}) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id(std::string(message_id));
  if (!task_id.empty()) request.mutable_message()->set_task_id(std::string(task_id));
  request.mutable_message()->add_parts()->set_text("hello");
  return request;
}

lf::a2a::v1::TaskPushNotificationConfig MakePushConfig(std::string_view task_id, std::string_view config_id) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(task_id));
  config.set_id(std::string(config_id));
  config.set_url("http://127.0.0.1/fake-push-callback");
  return config;
}

class ScenarioHarness final {
 public:
  ScenarioHarness() {
    options_.push_delivery = &delivery_;
    executor_ = std::make_unique<a2a::examples::ExampleExecutor>(std::move(options_));
    existing_task_id_ = SeedTask("existing-seed");
    subscribe_task_id_ = SeedTask("subscribe-seed");
  }

  bool Execute(std::string_view scenario, int index) {
    std::lock_guard<std::mutex> lock(mutex_);
    a2a::server::RequestContext context;
    if (scenario == "SendMessage_CreateTask")
      return executor_->SendMessage(MakeSendRequest(BuildId("create", index)), context).ok();
    if (scenario == "GetTask_ExistingTask") {
      lf::a2a::v1::GetTaskRequest request;
      request.set_id(existing_task_id_);
      return executor_->GetTask(request, context).ok();
    }
    if (scenario == "CancelTask_WorkingTask") {
      const std::string task_id = SeedTask(BuildId("cancel", index));
      lf::a2a::v1::CancelTaskRequest request;
      request.set_id(task_id);
      return executor_->CancelTask(request, context).ok();
    }
    if (scenario == "ListTasks_NoPagination" || scenario == "ListTasks_WithPagination") {
      a2a::server::ListTasksRequest request;
      if (scenario == "ListTasks_WithPagination") request.page_size = kListPageSize;
      return executor_->ListTasks(request, context).ok();
    }
    if (scenario == "SendMessage_FollowUpExistingTask")
      return executor_->SendMessage(MakeSendRequest(BuildId("follow", index), existing_task_id_), context).ok();
    if (scenario == "GetTask_MissingTaskError") {
      lf::a2a::v1::GetTaskRequest request;
      request.set_id(BuildId("missing", index));
      return !executor_->GetTask(request, context).ok();
    }
    if (scenario == "SendStreamingMessage_FiniteStream") {
      auto stream = executor_->SendStreamingMessage(MakeSendRequest(BuildId("stream", index)), context);
      return stream.ok() && DrainStream(stream.value().get());
    }
    if (scenario == "SubscribeToTask_FirstEventLatency") return SubscribeOnce(context);
    if (scenario == "SubscribeToTask_MultiSubscriber")
      return SubscribeOnce(context) && SubscribeOnce(context) && SubscribeOnce(context);
    if (scenario == "SubscribeToTask_TerminalCompletionLatency") {
      auto stream = executor_->SendStreamingMessage(MakeSendRequest(BuildId("terminal", index)), context);
      return stream.ok() && DrainStream(stream.value().get());
    }
    if (scenario == "SubscribeToTask_DisconnectOneSubscriber") return SubscribeOnce(context) && SubscribeOnce(context);
    if (scenario == "PushConfig_Create")
      return executor_
          ->CreateTaskPushNotificationConfig(MakePushConfig(existing_task_id_, BuildId("cfg-create", index)), context)
          .ok();
    if (scenario == "PushConfig_Get") {
      SeedPushConfig(existing_task_id_, "cfg-get");
      lf::a2a::v1::GetTaskPushNotificationConfigRequest request;
      request.set_task_id(existing_task_id_);
      request.set_id("cfg-get");
      return executor_->GetTaskPushNotificationConfig(request, context).ok();
    }
    if (scenario == "PushConfig_List") {
      SeedPushConfig(existing_task_id_, "cfg-list");
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
      request.set_task_id(existing_task_id_);
      return executor_->ListTaskPushNotificationConfigs(request, context).ok();
    }
    if (scenario == "PushConfig_Delete") {
      const std::string config_id = BuildId("cfg-delete", index);
      SeedPushConfig(existing_task_id_, config_id);
      lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request;
      request.set_task_id(existing_task_id_);
      request.set_id(config_id);
      return executor_->DeleteTaskPushNotificationConfig(request, context).ok();
    }
    if (scenario == "PushNotify_ManyConfigsOneTaskUpdate" || scenario == "PushDelivery_CallbackLatency") {
      for (int config = 0; config < kPushConfigFanout; ++config)
        SeedPushConfig(existing_task_id_, BuildId("fanout", config));
      return executor_->SendMessage(MakeSendRequest(BuildId("notify", index), existing_task_id_), context).ok();
    }
    return false;
  }

 private:
  static std::string BuildId(std::string_view prefix, int index) {
    std::string value;
    value.reserve(prefix.size() + 16U);
    value.append(prefix);
    value.push_back('-');
    value.append(std::to_string(index));
    return value;
  }
  std::string SeedTask(std::string_view message_id) {
    a2a::server::RequestContext context;
    auto response = executor_->SendMessage(MakeSendRequest(message_id), context);
    if (response.ok() && response.value().has_task()) return response.value().task().id();
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
      if (!next.ok()) return false;
      if (!next.value().has_value()) return true;
    }
  }

  RecordingPushDelivery delivery_;
  a2a::examples::ExampleExecutorOptions options_;
  std::unique_ptr<a2a::examples::ExampleExecutor> executor_;
  std::string existing_task_id_;
  std::string subscribe_task_id_;
  std::mutex mutex_;
};

ScenarioResult RunScenario(const Options& options, const std::string& scenario) {
  ScenarioHarness harness;
  const auto warmup_end = std::chrono::steady_clock::now() + std::chrono::duration<double>(options.warmup_seconds);
  int warmup_index = 0;
  while (std::chrono::steady_clock::now() < warmup_end) (void)harness.Execute(scenario, warmup_index++);
  ScenarioResult result;
  result.scenario = scenario;
  result.operations = options.requests;
  result.latencies.reserve(static_cast<std::size_t>(options.requests));
  std::mutex result_mutex;
  const auto started = std::chrono::steady_clock::now();
  auto operation = [&](int index) {
    const auto op_started = std::chrono::steady_clock::now();
    const bool ok = harness.Execute(scenario, index);
    const auto op_finished = std::chrono::steady_clock::now();
    const double latency =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(op_finished - op_started).count()) /
        kNanosecondsPerMillisecond;
    std::lock_guard<std::mutex> lock(result_mutex);
    if (ok) {
      ++result.success;
      result.latencies.push_back(latency);
    } else {
      ++result.errors;
    }
  };
  std::vector<std::future<void>> futures;
  futures.reserve(static_cast<std::size_t>(options.concurrency));
  for (int index = 0; index < options.requests; ++index) {
    futures.push_back(std::async(std::launch::async, operation, index));
    if (static_cast<int>(futures.size()) >= options.concurrency) {
      futures.front().get();
      futures.erase(futures.begin());
    }
  }
  for (auto& future : futures) future.get();
  const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  result.throughput = static_cast<double>(result.success) / std::max(elapsed, 0.000001);
  std::sort(result.latencies.begin(), result.latencies.end());
  return result;
}

bool ParseArgs(int argc, char** argv, Options* options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--transport" && index + 1 < argc)
      options->transport = argv[++index];
    else if (arg == "--store-backend" && index + 1 < argc)
      options->store_backend = argv[++index];
    else if (arg == "--requests" && index + 1 < argc)
      options->requests = std::atoi(argv[++index]);
    else if (arg == "--concurrency" && index + 1 < argc)
      options->concurrency = std::atoi(argv[++index]);
    else if (arg == "--warmup-seconds" && index + 1 < argc)
      options->warmup_seconds = std::atof(argv[++index]);
    else if (arg == "--duration-seconds" && index + 1 < argc)
      options->duration_seconds = std::atof(argv[++index]);
    else {
      std::cerr << "unknown or incomplete argument: " << arg << '\n';
      return false;
    }
  }
  return options->requests > 0 && options->concurrency > 0;
}

void WriteResultJson(const Options& options, const ScenarioResult& result, bool first) {
  const double p50 = Percentile(result.latencies, 50.0);
  const double p90 = Percentile(result.latencies, 90.0);
  const double p95 = Percentile(result.latencies, 95.0);
  const double p99 = Percentile(result.latencies, 99.0);
  const double max = result.latencies.empty() ? 0.0 : result.latencies.back();
  if (!first) std::cout << ",\n";
  std::cout << "  {\"scenario\":\"" << JsonEscape(result.scenario) << "\",\"transport\":\""
            << JsonEscape(options.transport) << "\",\"store_backend\":\"" << JsonEscape(options.store_backend)
            << "\",\"concurrency\":" << options.concurrency << ",\"operations\":" << result.operations
            << ",\"success\":" << result.success << ",\"errors\":" << result.errors
            << ",\"throughput_ops_per_sec\":" << std::fixed << std::setprecision(6) << result.throughput
            << ",\"warmup_seconds\":" << options.warmup_seconds << ",\"duration_seconds\":" << options.duration_seconds
            << ",\"driver_type\":\"" << kDriverType << "\",\"transport_path\":\"sdk_" << JsonEscape(options.transport)
            << "_server_dispatch\",\"latency_ms\":{\"p50\":" << p50 << ",\"p90\":" << p90 << ",\"p95\":" << p95
            << ",\"p99\":" << p99 << ",\"max\":" << max << "}}";
}
}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) return 2;
  if (options.store_backend == kPostgresStore) {
    std::cerr << "postgres performance backend requires a PostgreSQL-enabled driver build and A2A_TEST_POSTGRES_DSN; "
                 "this binary was built without that backend\n";
    return 3;
  }
  if (options.store_backend != kInMemoryStore ||
      (options.transport != kGrpcTransport && options.transport != kJsonRpcTransport &&
       options.transport != kHttpJsonTransport)) {
    std::cerr << "unsupported transport or store backend\n";
    return 2;
  }
  std::cout << "[\n";
  bool first = true;
  for (const auto& scenario : Scenarios()) {
    WriteResultJson(options, RunScenario(options, scenario), first);
    first = false;
  }
  std::cout << "\n]\n";
  return 0;
}
