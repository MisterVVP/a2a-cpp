// SPDX-License-Identifier: Apache-2.0

#include <google/protobuf/struct.pb.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/grpc_transport.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/protojson.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif
#include "a2a_performance_driver.h"

namespace {

using namespace a2a::tests::performance;

constexpr std::string_view kWireDriverType = "wire_tck_sut";
constexpr std::string_view kHostDefault = "127.0.0.1";
constexpr int kEndpointReserveSlack = 32;
constexpr int kListFixtureTaskCount = 20;
constexpr std::string_view kTckRequiredExtensionUri = "urn:a2a:tck:required-extension";
constexpr std::string_view kA2aVersionHeader = "A2A-Version";
constexpr std::string_view kA2aVersion = "1.0";
constexpr std::chrono::milliseconds kWireStreamWaitTimeout{5000};
constexpr int kFocusedListConfigCount = 3;
constexpr std::string_view kScenarioSendStreamingMessageFiniteStreamSharedClient =
    "SendStreamingMessage_FiniteStream_SharedClient";
constexpr std::string_view kScenarioSubscribeToTaskFirstEventLatencySharedClient =
    "SubscribeToTask_FirstEventLatency_SharedClient";

struct FocusedWireFixture final {
  std::string task_id;
  std::string config_id;
  std::vector<DeleteFixture> delete_fixtures;
};

class CountingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    (void)response;
    std::lock_guard lock(mutex_);
    if (events_ == 0) {
      first_event_at_ = std::chrono::steady_clock::now();
    }
    ++events_;
    ready_.notify_all();
  }

  void OnError(const a2a::core::Error& error) override {
    (void)error;
    std::lock_guard lock(mutex_);
    ++errors_;
    completed_ = true;
    ready_.notify_all();
  }

  void OnCompleted() override {
    std::lock_guard lock(mutex_);
    completed_at_ = std::chrono::steady_clock::now();
    completed_ = true;
    ready_.notify_all();
  }

  [[nodiscard]] bool WaitForEventCount(int expected_events) {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, kWireStreamWaitTimeout,
                           [this, expected_events]() { return events_ >= expected_events || completed_; }) &&
           errors_ == 0 && events_ >= expected_events;
  }

  [[nodiscard]] bool WaitForCompletion() {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, kWireStreamWaitTimeout, [this]() { return completed_; }) && errors_ == 0 &&
           events_ > 0;
  }

  [[nodiscard]] int event_count() const {
    std::lock_guard lock(mutex_);
    return events_;
  }

  [[nodiscard]] double FirstEventLatencySince(std::chrono::steady_clock::time_point started) const {
    std::lock_guard lock(mutex_);
    if (events_ == 0) {
      return 0.0;
    }
    return DurationMillis(first_event_at_ - started);
  }

  [[nodiscard]] double CompletionLatencySince(std::chrono::steady_clock::time_point started) const {
    std::lock_guard lock(mutex_);
    if (!completed_) {
      return 0.0;
    }
    return DurationMillis(completed_at_ - started);
  }

 private:
  [[nodiscard]] static double DurationMillis(std::chrono::steady_clock::duration duration) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count()) /
           kNanosecondsPerMillisecond;
  }

  mutable std::mutex mutex_;
  std::condition_variable ready_;
  int events_ = 0;
  int errors_ = 0;
  bool completed_ = false;
  std::chrono::steady_clock::time_point first_event_at_;
  std::chrono::steady_clock::time_point completed_at_;
};

struct WireOptions final {
  std::string transport = std::string(kGrpcTransport);
  std::string store_backend = std::string(kInMemoryStore);
  std::string host = std::string(kHostDefault);
  int port = 0;
  int requests = kDefaultRequests;
  int concurrency = kDefaultConcurrency;
  double warmup_seconds = 0.0;
  double duration_seconds = 0.0;
  std::vector<std::string> scenarios;
};

std::string HttpEndpoint(const WireOptions& options, std::string_view path) {
  std::string endpoint;
  endpoint.reserve(options.host.size() + path.size() + kEndpointReserveSlack);
  endpoint.append("http://");
  endpoint.append(options.host);
  endpoint.push_back(':');
  endpoint.append(std::to_string(options.port));
  endpoint.append(path);
  return endpoint;
}

std::string GrpcEndpoint(const WireOptions& options) {
  std::string endpoint;
  endpoint.reserve(options.host.size() + kEndpointReserveSlack);
  endpoint.append(options.host);
  endpoint.push_back(':');
  endpoint.append(std::to_string(options.port + 1));
  return endpoint;
}

a2a::client::ResolvedInterface MakeResolvedInterface(const WireOptions& options) {
  if (options.transport == kGrpcTransport) {
    return {.transport = a2a::client::PreferredTransport::kGrpc,
            .url = GrpcEndpoint(options),
            .security_requirements = {},
            .security_schemes = {}};
  }
  if (options.transport == kJsonRpcTransport) {
    return {.transport = a2a::client::PreferredTransport::kJsonRpc,
            .url = HttpEndpoint(options, "/rpc"),
            .security_requirements = {},
            .security_schemes = {}};
  }
  return {.transport = a2a::client::PreferredTransport::kRest,
          .url = HttpEndpoint(options, "/a2a"),
          .security_requirements = {},
          .security_schemes = {}};
}

a2a::client::CallOptions MakeCallOptions() {
  a2a::client::CallOptions options;
  options.headers.emplace(std::string(kA2aVersionHeader), std::string(kA2aVersion));
  options.extensions.emplace_back(kTckRequiredExtensionUri);
  return options;
}

std::unique_ptr<a2a::client::A2AClient> MakeClient(const WireOptions& options) {
  a2a::client::ResolvedInterface resolved = MakeResolvedInterface(options);
  if (options.transport == kGrpcTransport) {
    auto channel = grpc::CreateChannel(resolved.url, grpc::InsecureChannelCredentials());
    return std::make_unique<a2a::client::A2AClient>(
        std::make_unique<a2a::client::GrpcTransport>(std::move(resolved), std::move(channel)));
  }
  if (options.transport == kJsonRpcTransport) {
    return std::make_unique<a2a::client::A2AClient>(a2a::client::JsonRpcTransport::CreateDefault(std::move(resolved)));
  }
  return std::make_unique<a2a::client::A2AClient>(a2a::client::HttpJsonTransport::CreateDefault(std::move(resolved)));
}

std::string SeedTask(a2a::client::A2AClient* client, std::string_view message_id,
                     const a2a::client::CallOptions& call_options) {
  auto response = client->SendMessage(MakeSendRequest(message_id), call_options);
  if (response.ok() && response.value().has_task()) {
    return response.value().task().id();
  }
  return {};
}

bool IsListScenario(std::string_view scenario) {
  return scenario == kScenarioListTasksNoPagination || scenario == kScenarioListTasksWithPagination;
}

bool SeedListFixture(a2a::client::A2AClient* client, const a2a::client::CallOptions& call_options) {
  for (int task_index = 0; task_index < kListFixtureTaskCount; ++task_index) {
    if (SeedTask(client, BuildId("wire-list-fixture", task_index), call_options).empty()) {
      return false;
    }
  }
  return true;
}

OperationOutcome ExecuteWireStreaming(a2a::client::A2AClient* client, int index,
                                      const a2a::client::CallOptions& call_options) {
  CountingObserver observer;
  const auto started = std::chrono::steady_clock::now();
  auto stream = client->SendStreamingMessage(MakeSendRequest(BuildId("wire-stream", index)), observer, call_options);
  const bool completed = stream.ok() && observer.WaitForCompletion();
  return {.ok = completed,
          .event_count = observer.event_count(),
          .first_event_latency_ms = observer.FirstEventLatencySince(started),
          .completion_latency_ms = observer.CompletionLatencySince(started)};
}

OperationOutcome ExecuteWireSubscribeFirstEvent(a2a::client::A2AClient* client, int index,
                                                const a2a::client::CallOptions& call_options) {
  const std::string task_id = SeedTask(client, BuildId("wire-subscribe-seed", index), call_options);
  if (task_id.empty()) {
    return {};
  }
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(task_id);
  CountingObserver observer;
  const auto started = std::chrono::steady_clock::now();
  auto stream = client->SubscribeTask(request, observer, call_options);
  if (!stream.ok() || !observer.WaitForEventCount(1)) {
    if (stream.ok()) {
      stream.value()->Cancel();
    }
    return {};
  }
  const double first_event_latency_ms = observer.FirstEventLatencySince(started);
  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id(task_id);
  const bool cancelled = client->CancelTask(cancel_request, call_options).ok();
  const bool completed = observer.WaitForCompletion();
  if (!completed) {
    stream.value()->Cancel();
  }
  return {.ok = cancelled && completed,
          .event_count = observer.event_count(),
          .first_event_latency_ms = first_event_latency_ms,
          .completion_latency_ms = observer.CompletionLatencySince(started)};
}

OperationOutcome ExecuteIdleStreamClientCancellation(a2a::client::A2AClient* client, int index,
                                                     const a2a::client::CallOptions& call_options) {
  const std::string task_id = SeedTask(client, BuildId("wire-idle-cancel-seed", index), call_options);
  if (task_id.empty()) {
    return {};
  }
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(task_id);
  CountingObserver observer;
  auto stream = client->SubscribeTask(request, observer, call_options);
  if (!stream.ok() || !observer.WaitForEventCount(1)) {
    if (stream.ok()) {
      stream.value()->Cancel();
    }
    return {};
  }

  const auto cancellation_started = std::chrono::steady_clock::now();
  stream.value()->Cancel();
  const auto cancellation_duration = std::chrono::steady_clock::now() - cancellation_started;
  const double cancellation_latency_ms =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(cancellation_duration).count()) /
      kNanosecondsPerMillisecond;
  return {.ok = true, .event_count = observer.event_count(), .measured_latency_ms = cancellation_latency_ms};
}

bool ExecuteWirePushCreate(a2a::client::A2AClient* client, int index, std::string_view task_id,
                           const a2a::client::CallOptions& call_options) {
  auto config =
      client->CreateTaskPushNotificationConfig(MakePushConfig(task_id, BuildId("wire-cfg", index)), call_options);
  return config.ok() && config.value().task_id() == task_id;
}

bool SeedWirePushConfig(a2a::client::A2AClient* client, std::string_view task_id, std::string_view config_id,
                        const a2a::client::CallOptions& call_options) {
  return client->CreateTaskPushNotificationConfig(MakePushConfig(task_id, config_id), call_options).ok();
}

bool ExecuteWirePushGet(a2a::client::A2AClient* client, std::string_view task_id, std::string_view config_id,
                        const a2a::client::CallOptions& call_options) {
  lf::a2a::v1::GetTaskPushNotificationConfigRequest request;
  request.set_task_id(std::string(task_id));
  request.set_id(std::string(config_id));
  auto config = client->GetTaskPushNotificationConfig(request, call_options);
  return config.ok() && config.value().id() == config_id;
}

bool ExecuteWirePushList(a2a::client::A2AClient* client, std::string_view task_id,
                         const a2a::client::CallOptions& call_options) {
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
  request.set_task_id(std::string(task_id));
  return client->ListTaskPushNotificationConfigs(request, call_options).ok();
}

bool ExecuteWirePushDelete(a2a::client::A2AClient* client, std::string_view task_id, std::string_view config_id,
                           const a2a::client::CallOptions& call_options) {
  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request;
  request.set_task_id(std::string(task_id));
  request.set_id(std::string(config_id));
  return client->DeleteTaskPushNotificationConfig(request, call_options).ok();
}

OperationOutcome MakeOutcome(bool ok) { return {.ok = ok, .event_count = ok ? 1 : 0}; }

bool PrepareFocusedWireFixture(a2a::client::A2AClient* client, std::string_view scenario, int operation_count,
                               std::string_view fixture_prefix, const a2a::client::CallOptions& call_options,
                               FocusedWireFixture* fixture) {
  if (scenario == kScenarioPushConfigDelete) {
    fixture->delete_fixtures.reserve(static_cast<std::size_t>(operation_count));
    for (int index = 0; index < operation_count; ++index) {
      std::string task_id = SeedTask(client, BuildId(fixture_prefix, index), call_options);
      if (task_id.empty()) {
        return false;
      }
      std::string config_id = BuildId(fixture_prefix, index + 1);
      if (!SeedWirePushConfig(client, task_id, config_id, call_options)) {
        return false;
      }
      fixture->delete_fixtures.push_back({.task_id = std::move(task_id), .config_id = std::move(config_id)});
    }
    return true;
  }
  const bool needs_task = scenario == kScenarioGetTaskExistingTask || scenario == kScenarioPushConfigCreate ||
                          scenario == kScenarioPushConfigGet || scenario == kScenarioPushConfigList;
  if (!needs_task) {
    return true;
  }
  fixture->task_id = SeedTask(client, BuildId(fixture_prefix, 0), call_options);
  if (fixture->task_id.empty()) {
    return false;
  }
  if (scenario == kScenarioPushConfigGet) {
    fixture->config_id = BuildId(fixture_prefix, 1);
    return SeedWirePushConfig(client, fixture->task_id, fixture->config_id, call_options);
  }
  if (scenario == kScenarioPushConfigList) {
    for (int index = 0; index < kFocusedListConfigCount; ++index) {
      if (!SeedWirePushConfig(client, fixture->task_id, BuildId(fixture_prefix, index + 1), call_options)) {
        return false;
      }
    }
  }
  return true;
}

std::string SeedTaskAtHistoryDepth(a2a::client::A2AClient* client, std::string_view fixture_id, int history_depth,
                                   const a2a::client::CallOptions& call_options) {
  std::string task_id = SeedTask(client, BuildId(fixture_id, 0), call_options);
  for (int depth = 1; depth < history_depth && !task_id.empty(); ++depth) {
    if (!client->SendMessage(MakeSendRequest(BuildId(fixture_id, depth), task_id), call_options).ok()) {
      return {};
    }
  }
  return task_id;
}

OperationOutcome ExecuteScenario(a2a::client::A2AClient* client, std::string_view scenario, int index,
                                 const FocusedWireFixture& fixture, std::string_view follow_up_task_id = {}) {
  const a2a::client::CallOptions call_options = MakeCallOptions();
  if (scenario == kScenarioSendMessageCreateTask) {
    return MakeOutcome(client->SendMessage(MakeSendRequest(BuildId("wire-create", index)), call_options).ok());
  }
  if (scenario == kScenarioGetTaskExistingTask) {
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(fixture.task_id);
    return MakeOutcome(client->GetTask(request, call_options).ok());
  }
  if (scenario == kScenarioCancelTaskWorkingTask) {
    const std::string task_id = SeedTask(client, BuildId("wire-cancel-seed", index), call_options);
    if (task_id.empty()) {
      return {};
    }
    lf::a2a::v1::CancelTaskRequest request;
    request.set_id(task_id);
    return MakeOutcome(client->CancelTask(request, call_options).ok());
  }
  if (IsListScenario(scenario)) {
    (void)index;
    a2a::client::ListTasksRequest request;
    if (scenario == kScenarioListTasksWithPagination) {
      request.page_size = kListPageSize;
    }
    return MakeOutcome(client->ListTasks(request, call_options).ok());
  }
  if (ScenarioHistoryDepth(scenario) > 0) {
    if (follow_up_task_id.empty()) {
      return {};
    }
    return MakeOutcome(
        client->SendMessage(MakeSendRequest(BuildId("wire-follow-up", index), follow_up_task_id), call_options).ok());
  }
  if (scenario == kScenarioGetTaskMissingTaskError) {
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(BuildId("wire-missing", index));
    return MakeOutcome(!client->GetTask(request, call_options).ok());
  }
  if (scenario == kScenarioSendStreamingMessageFiniteStream ||
      scenario == kScenarioSendStreamingMessageFiniteStreamSharedClient) {
    return ExecuteWireStreaming(client, index, call_options);
  }
  if (scenario == kScenarioSubscribeToTaskFirstEventLatency ||
      scenario == kScenarioSubscribeToTaskFirstEventLatencySharedClient) {
    return ExecuteWireSubscribeFirstEvent(client, index, call_options);
  }
  if (scenario == kScenarioIdleStreamClientCancellationLatency) {
    return ExecuteIdleStreamClientCancellation(client, index, call_options);
  }
  if (scenario == kScenarioPushConfigCreate) {
    return MakeOutcome(ExecuteWirePushCreate(client, index, fixture.task_id, call_options));
  }
  if (scenario == kScenarioPushConfigGet) {
    return MakeOutcome(ExecuteWirePushGet(client, fixture.task_id, fixture.config_id, call_options));
  }
  if (scenario == kScenarioPushConfigList) {
    return MakeOutcome(ExecuteWirePushList(client, fixture.task_id, call_options));
  }
  if (scenario == kScenarioPushConfigDelete) {
    if (index < 0 || static_cast<std::size_t>(index) >= fixture.delete_fixtures.size()) {
      return {};
    }
    const DeleteFixture& delete_fixture = fixture.delete_fixtures[static_cast<std::size_t>(index)];
    return MakeOutcome(ExecuteWirePushDelete(client, delete_fixture.task_id, delete_fixture.config_id, call_options));
  }
  return {};
}

bool IsSharedClientWireScenario(std::string_view scenario) {
  return scenario == kScenarioSendStreamingMessageFiniteStreamSharedClient ||
         scenario == kScenarioSubscribeToTaskFirstEventLatencySharedClient;
}

bool UsesSharedHttpClient(std::string_view scenario) {
  return scenario == kScenarioIdleStreamClientCancellationLatency || IsSharedClientWireScenario(scenario);
}

bool IsStreamingWireScenario(std::string_view scenario) {
  return scenario == kScenarioSendStreamingMessageFiniteStream ||
         scenario == kScenarioSubscribeToTaskFirstEventLatency ||
         scenario == kScenarioIdleStreamClientCancellationLatency || IsSharedClientWireScenario(scenario);
}

std::vector<std::unique_ptr<a2a::client::A2AClient>> MakeMeasuredClients(const WireOptions& options, int client_count) {
  std::vector<std::unique_ptr<a2a::client::A2AClient>> clients;
  clients.reserve(static_cast<std::size_t>(client_count));
  for (int worker_index = 0; worker_index < client_count; ++worker_index) {
    clients.push_back(MakeClient(options));
  }
  return clients;
}

OperationOutcome ExecuteMeasuredWireOperation(const std::vector<std::unique_ptr<a2a::client::A2AClient>>& clients,
                                              const std::string& scenario,
                                              const std::vector<std::string>& follow_up_task_ids,
                                              const FocusedWireFixture& focused_fixture,
                                              bool use_shared_http_stream_client, int worker_index, int index) {
  const std::string_view task_id =
      follow_up_task_ids.empty() ? std::string_view{} : follow_up_task_ids[static_cast<std::size_t>(index)];
  const std::size_t client_index = use_shared_http_stream_client ? 0U : static_cast<std::size_t>(worker_index);
  return ExecuteScenario(clients[client_index].get(), scenario, index, focused_fixture, task_id);
}

ScenarioResult RunWireScenario(const WireOptions& options, const std::string& scenario) {
  const auto warmup_end = std::chrono::steady_clock::now() + std::chrono::duration<double>(options.warmup_seconds);
  int warmup_index = -1;
  auto warmup_client = MakeClient(options);
  while (std::chrono::steady_clock::now() < warmup_end) {
    const auto call_options = MakeCallOptions();
    FocusedWireFixture warmup_fixture;
    if (!PrepareFocusedWireFixture(warmup_client.get(), scenario, 1, BuildId("wire-warmup-fixture", warmup_index),
                                   call_options, &warmup_fixture)) {
      break;
    }
    const int history_depth = ScenarioHistoryDepth(scenario);
    if (history_depth > 0) {
      const std::string task_id = SeedTaskAtHistoryDepth(
          warmup_client.get(), BuildId("wire-follow-warmup", warmup_index), history_depth, call_options);
      (void)ExecuteScenario(warmup_client.get(), scenario, warmup_index, warmup_fixture, task_id);
    } else {
      const int operation_index = scenario == kScenarioPushConfigDelete ? 0 : warmup_index;
      (void)ExecuteScenario(warmup_client.get(), scenario, operation_index, warmup_fixture);
    }
    --warmup_index;
  }

  const int worker_count = std::min(options.concurrency, options.requests);
  const bool use_shared_http_client = UsesSharedHttpClient(scenario) && options.transport != kGrpcTransport;
  const int client_count = use_shared_http_client ? 1 : worker_count;
  auto clients = MakeMeasuredClients(options, client_count);
  const a2a::client::CallOptions fixture_call_options = MakeCallOptions();
  FocusedWireFixture focused_fixture;
  if (!PrepareFocusedWireFixture(clients.front().get(), scenario, options.requests, "wire-measured-fixture",
                                 fixture_call_options, &focused_fixture)) {
    ScenarioResult failed;
    failed.scenario = scenario;
    failed.operations = options.requests;
    failed.errors = options.requests;
    return failed;
  }
  if (IsListScenario(scenario)) {
    if (!SeedListFixture(clients.front().get(), fixture_call_options)) {
      ScenarioResult failed;
      failed.scenario = scenario;
      failed.operations = options.requests;
      failed.errors = options.requests;
      return failed;
    }
  }

  std::vector<std::string> follow_up_task_ids;
  const int history_depth = ScenarioHistoryDepth(scenario);
  if (history_depth > 0) {
    follow_up_task_ids.reserve(static_cast<std::size_t>(options.requests));
    const a2a::client::CallOptions call_options = MakeCallOptions();
    for (int index = 0; index < options.requests; ++index) {
      std::string task_id = SeedTaskAtHistoryDepth(warmup_client.get(), BuildId("wire-follow-fixture", index),
                                                   history_depth, call_options);
      if (task_id.empty()) {
        ScenarioResult failed;
        failed.scenario = scenario;
        failed.operations = options.requests;
        failed.errors = options.requests;
        return failed;
      }
      follow_up_task_ids.push_back(std::move(task_id));
    }
  }

  if (use_shared_http_client) {
    warmup_client.reset();
  }
  ScenarioResult result =
      RunMeasuredScenario(scenario, options.requests, options.concurrency, options.duration_seconds,
                          [&clients, &scenario, &follow_up_task_ids, &focused_fixture, use_shared_http_client](
                              int worker_index, int index) {
                            return ExecuteMeasuredWireOperation(clients, scenario, follow_up_task_ids, focused_fixture,
                                                                use_shared_http_client, worker_index, index);
                          });
#if defined(__linux__)
  result.client_process_thread_count = static_cast<int>(
      std::distance(std::filesystem::directory_iterator("/proc/self/task"), std::filesystem::directory_iterator{}));
#endif
  return result;
}

bool IsPushConfigWireScenario(std::string_view scenario) {
  return scenario == kScenarioPushConfigCreate || scenario == kScenarioPushConfigGet ||
         scenario == kScenarioPushConfigList || scenario == kScenarioPushConfigDelete;
}

bool IsWireScenario(std::string_view scenario, std::string_view transport) {
  if (IsSharedClientWireScenario(scenario)) {
    return transport != kGrpcTransport;
  }
  const bool supported_core = scenario == kScenarioSendMessageCreateTask || scenario == kScenarioGetTaskExistingTask ||
                              scenario == kScenarioCancelTaskWorkingTask ||
                              scenario == kScenarioListTasksNoPagination ||
                              scenario == kScenarioListTasksWithPagination || ScenarioHistoryDepth(scenario) > 0 ||
                              scenario == kScenarioGetTaskMissingTaskError;
  if (supported_core) {
    return true;
  }
  return IsStreamingWireScenario(scenario) || IsPushConfigWireScenario(scenario);
}

bool ParseScenarios(std::string_view value, WireOptions* options) {
  options->scenarios = SplitCsv(value);
  if (options->scenarios.empty()) {
    std::cerr << "scenario selection must not be empty\n";
    return false;
  }
  return true;
}

bool ParseArgs(int argc, char** argv, WireOptions* options) {
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
    } else if (arg == "--host") {
      options->host = value;
    } else if (arg == "--port") {
      options->port = std::atoi(raw_value);
    } else if (arg == "--requests") {
      options->requests = std::atoi(raw_value);
    } else if (arg == "--concurrency") {
      options->concurrency = std::atoi(raw_value);
    } else if (arg == "--warmup-seconds") {
      options->warmup_seconds = std::atof(raw_value);
    } else if (arg == "--duration-seconds") {
      options->duration_seconds = std::atof(raw_value);
    } else if (arg == "--scenarios") {
      if (!ParseScenarios(value, options)) {
        return false;
      }
    } else {
      std::cerr << "unknown or incomplete argument: " << arg << '\n';
      return false;
    }
  }
  for (const std::string& scenario : options->scenarios) {
    if (!IsWireScenario(scenario, options->transport)) {
      std::cerr << "unsupported wire scenario: " << scenario << '\n';
      return false;
    }
  }
  return options->requests > 0 && options->concurrency > 0 && options->port > 0;
}

std::vector<std::string> SelectedScenarios(const WireOptions& options) {
  if (!options.scenarios.empty()) {
    return options.scenarios;
  }
  std::vector<std::string> scenarios = {std::string(kScenarioListTasksNoPagination),
                                        std::string(kScenarioListTasksWithPagination),
                                        std::string(kScenarioSendMessageCreateTask),
                                        std::string(kScenarioGetTaskExistingTask),
                                        std::string(kScenarioCancelTaskWorkingTask),
                                        std::string(kScenarioSendMessageFollowUpExistingTask),
                                        std::string(kScenarioSendMessageFollowUpAtHistoryDepth),
                                        std::string(kScenarioGetTaskMissingTaskError),
                                        std::string(kScenarioSendStreamingMessageFiniteStream),
                                        std::string(kScenarioSubscribeToTaskFirstEventLatency),
                                        std::string(kScenarioIdleStreamClientCancellationLatency),
                                        std::string(kScenarioPushConfigCreate),
                                        std::string(kScenarioPushConfigGet),
                                        std::string(kScenarioPushConfigList),
                                        std::string(kScenarioPushConfigDelete)};
  if (options.transport != kGrpcTransport) {
    scenarios.push_back(std::string(kScenarioSendStreamingMessageFiniteStreamSharedClient));
    scenarios.push_back(std::string(kScenarioSubscribeToTaskFirstEventLatencySharedClient));
  }
  return scenarios;
}

std::string TransportPath(std::string_view transport) {
  if (transport == kGrpcTransport) {
    return "wire_grpc";
  }
  if (transport == kJsonRpcTransport) {
    return "wire_jsonrpc";
  }
  return "wire_http_json";
}

google::protobuf::Struct BuildResultObject(const WireOptions& options, const ScenarioResult& result
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
                                           ,
                                           const a2a::core::subscription_diagnostics::Snapshot& client_diagnostics
#endif
) {
  google::protobuf::Struct object;
  PopulateCommonResultFields(&object, result.scenario, options.transport, options.store_backend, options.concurrency,
                             result);
  SetIntegerField(&object, "configured_requests", options.requests);
  const int history_depth = ScenarioHistoryDepth(result.scenario);
  if (history_depth > 0) {
    SetIntegerField(&object, "history_depth", history_depth);
  }
  SetNumberField(&object, "configured_duration_seconds", options.duration_seconds);
  SetNumberField(&object, "measured_duration_seconds", result.measured_duration_seconds);
  if (IsStreamingWireScenario(result.scenario) && options.transport != kGrpcTransport) {
    SetIntegerField(&object, "client_process_thread_count", result.client_process_thread_count);
  }
  SetStringField(&object, "driver_type", kWireDriverType);
  SetStringField(&object, "transport_path", TransportPath(options.transport));
  AddLatencyField(&object, result);
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  google::protobuf::Struct diagnostics;
  for (std::size_t index = 0; index < client_diagnostics.size(); ++index) {
    google::protobuf::Struct aggregate;
    SetNumberField(&aggregate, "count", static_cast<double>(client_diagnostics[index].count));
    SetNumberField(&aggregate, "total_ns", static_cast<double>(client_diagnostics[index].elapsed_nanoseconds));
    SetNumberField(&aggregate, "max_ns", static_cast<double>(client_diagnostics[index].maximum_nanoseconds));
    (*diagnostics.mutable_fields())[std::string(a2a::core::subscription_diagnostics::kPhaseNames[index])]
        .mutable_struct_value()
        ->Swap(&aggregate);
  }
  (*object.mutable_fields())["client_subscription_diagnostics"].mutable_struct_value()->Swap(&diagnostics);
#endif
  return object;
}

void WriteResultJson(const WireOptions& options, const ScenarioResult& result, bool first
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
                     ,
                     const a2a::core::subscription_diagnostics::Snapshot& client_diagnostics
#endif
) {
  if (!first) {
    std::cout << ",\n";
  }
  const auto json = a2a::core::MessageToJson(BuildResultObject(options, result
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
                                                               ,
                                                               client_diagnostics
#endif
                                                               ));
  std::cout << "  " << (json.ok() ? json.value() : "{}");
}

}  // namespace

int main(int argc, char** argv) {
  WireOptions options;
  if (!ParseArgs(argc, argv, &options)) {
    return kUsageExitCode;
  }
  if ((options.transport != kGrpcTransport && options.transport != kJsonRpcTransport &&
       options.transport != kHttpJsonTransport) ||
      (options.store_backend != kInMemoryStore && options.store_backend != kPostgresStore)) {
    std::cerr << "unsupported transport or store backend\n";
    return kUsageExitCode;
  }
  std::cout << "[\n";
  bool first = true;
  for (const std::string& scenario : SelectedScenarios(options)) {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    (void)a2a::core::subscription_diagnostics::TakeSnapshot();
#endif
    const auto result = RunWireScenario(options, scenario);
    WriteResultJson(options, result, first
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
                    ,
                    a2a::core::subscription_diagnostics::TakeSnapshot()
#endif
    );
    first = false;
  }
  std::cout << "\n]\n";
  return 0;
}
