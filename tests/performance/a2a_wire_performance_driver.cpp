// SPDX-License-Identifier: Apache-2.0

#include <google/protobuf/struct.pb.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/grpc_transport.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/protojson.h"
#include "a2a_performance_driver.h"

namespace {

using namespace a2a::tests::performance;

constexpr std::string_view kWireDriverType = "wire_tck_sut";
constexpr std::string_view kHostDefault = "127.0.0.1";
constexpr int kEndpointReserveSlack = 32;
constexpr int kListFixtureTaskCount = 20;
constexpr std::string_view kTckRequiredExtensionUri = "urn:a2a:tck:required-extension";

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

bool ExecuteScenario(a2a::client::A2AClient* client, std::string_view scenario, int index) {
  const a2a::client::CallOptions call_options = MakeCallOptions();
  if (scenario == kScenarioSendMessageCreateTask) {
    return client->SendMessage(MakeSendRequest(BuildId("wire-create", index)), call_options).ok();
  }
  if (scenario == kScenarioGetTaskExistingTask) {
    const std::string task_id = SeedTask(client, BuildId("wire-get-seed", index), call_options);
    if (task_id.empty()) {
      return false;
    }
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(task_id);
    return client->GetTask(request, call_options).ok();
  }
  if (scenario == kScenarioCancelTaskWorkingTask) {
    const std::string task_id = SeedTask(client, BuildId("wire-cancel-seed", index), call_options);
    if (task_id.empty()) {
      return false;
    }
    lf::a2a::v1::CancelTaskRequest request;
    request.set_id(task_id);
    return client->CancelTask(request, call_options).ok();
  }
  if (IsListScenario(scenario)) {
    (void)index;
    a2a::client::ListTasksRequest request;
    if (scenario == kScenarioListTasksWithPagination) {
      request.page_size = kListPageSize;
    }
    return client->ListTasks(request, call_options).ok();
  }
  if (scenario == kScenarioSendMessageFollowUpExistingTask) {
    const std::string task_id = SeedTask(client, BuildId("wire-follow-seed", index), call_options);
    if (task_id.empty()) {
      return false;
    }
    return client->SendMessage(MakeSendRequest(BuildId("wire-follow-up", index), task_id), call_options).ok();
  }
  if (scenario == kScenarioGetTaskMissingTaskError) {
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(BuildId("wire-missing", index));
    return !client->GetTask(request, call_options).ok();
  }
  return false;
}

ScenarioResult RunWireScenario(const WireOptions& options, const std::string& scenario) {
  const auto warmup_end = std::chrono::steady_clock::now() + std::chrono::duration<double>(options.warmup_seconds);
  int warmup_index = 0;
  auto warmup_client = MakeClient(options);
  while (std::chrono::steady_clock::now() < warmup_end) {
    (void)ExecuteScenario(warmup_client.get(), scenario, warmup_index++);
  }

  const int worker_count = std::min(options.concurrency, options.requests);
  std::vector<std::unique_ptr<a2a::client::A2AClient>> clients;
  clients.reserve(static_cast<std::size_t>(worker_count));
  for (int worker_index = 0; worker_index < worker_count; ++worker_index) {
    clients.push_back(MakeClient(options));
  }
  if (IsListScenario(scenario)) {
    const a2a::client::CallOptions call_options = MakeCallOptions();
    if (!SeedListFixture(clients.front().get(), call_options)) {
      ScenarioResult failed;
      failed.scenario = scenario;
      failed.operations = options.requests;
      failed.errors = options.requests;
      return failed;
    }
  }

  return RunMeasuredScenario(
      scenario, options.requests, options.concurrency, [&clients, &scenario](int worker_index, int index) {
        return ExecuteScenario(clients[static_cast<std::size_t>(worker_index)].get(), scenario, index);
      });
}

bool IsWireScenario(std::string_view scenario) {
  return scenario == kScenarioSendMessageCreateTask || scenario == kScenarioGetTaskExistingTask ||
         scenario == kScenarioCancelTaskWorkingTask || scenario == kScenarioListTasksNoPagination ||
         scenario == kScenarioListTasksWithPagination || scenario == kScenarioSendMessageFollowUpExistingTask ||
         scenario == kScenarioGetTaskMissingTaskError;
}

bool ParseScenarios(std::string_view value, WireOptions* options) {
  options->scenarios = SplitCsv(value);
  if (options->scenarios.empty()) {
    std::cerr << "scenario selection must not be empty\n";
    return false;
  }
  for (const std::string& scenario : options->scenarios) {
    if (!IsWireScenario(scenario)) {
      std::cerr << "unsupported wire scenario: " << scenario << '\n';
      return false;
    }
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
  return options->requests > 0 && options->concurrency > 0 && options->port > 0;
}

std::vector<std::string> SelectedScenarios(const WireOptions& options) {
  if (!options.scenarios.empty()) {
    return options.scenarios;
  }
  return {std::string(kScenarioListTasksNoPagination),  std::string(kScenarioListTasksWithPagination),
          std::string(kScenarioSendMessageCreateTask),  std::string(kScenarioGetTaskExistingTask),
          std::string(kScenarioCancelTaskWorkingTask),  std::string(kScenarioSendMessageFollowUpExistingTask),
          std::string(kScenarioGetTaskMissingTaskError)};
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

google::protobuf::Struct BuildResultObject(const WireOptions& options, const ScenarioResult& result) {
  google::protobuf::Struct object;
  PopulateCommonResultFields(&object, result.scenario, options.transport, options.store_backend, options.concurrency,
                             result);
  SetStringField(&object, "driver_type", kWireDriverType);
  SetStringField(&object, "transport_path", TransportPath(options.transport));
  AddLatencyField(&object, result);
  return object;
}

void WriteResultJson(const WireOptions& options, const ScenarioResult& result, bool first) {
  if (!first) {
    std::cout << ",\n";
  }
  const auto json = a2a::core::MessageToJson(BuildResultObject(options, result));
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
    WriteResultJson(options, RunWireScenario(options, scenario), first);
    first = false;
  }
  std::cout << "\n]\n";
  return 0;
}
