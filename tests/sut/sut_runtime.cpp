// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/sut_runtime.h"

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <utility>

#include "a2a/server/network_utils.h"
#include "a2a/server/server_bindings.h"
#include "sut/sut_agent.h"
#include "sut/tck_sut_http_server.h"
#include "sut/tck_sut_store.h"
#ifdef A2A_SUT_ASYNC
#include <boost/asio/thread_pool.hpp>

#include "a2a/core/http_constants.h"
#include "a2a/core/protojson.h"
#include "a2a/server/engine/async_runtime.h"
#include "a2a/server/event_http_dispatcher.h"
#include "a2a/server/http_server_response_builder.h"
#include "sut/pipeline_read_fixture.h"
#include "sut/sut_coroutine_executor.h"
#endif

namespace a2a::tests::sut {
namespace {
struct SutServerConfig final {
  SutConfig listener;
  SutEndpoints endpoints;
};
constexpr int kMaxHttpPort = 65534;
constexpr std::time_t kAgentCardLastModifiedUnix = 1704067200;
constexpr auto kCacheControl = "public, max-age=300";
constexpr auto kInvalidTls = "SUT TLS requires readable certificate and key files";
constexpr auto kHttpJoined = " shutdown: HTTP connection threads joined\n";
volatile std::sig_atomic_t kKeepRunning = 1;
void SignalHandler([[maybe_unused]] int signal_number) { kKeepRunning = 0; }
void SetSignals() {
  kKeepRunning = 1;
  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#else
  std::signal(SIGBREAK, SignalHandler);
#endif
}
std::shared_ptr<::grpc::ServerCredentials> GrpcCredentials() {
  const char* certificate_path = std::getenv(kTlsCertificateFileEnv);
  const char* key_path = std::getenv(kTlsKeyFileEnv);
  if (certificate_path == nullptr && key_path == nullptr) {
    return ::grpc::InsecureServerCredentials();
  }
  if (certificate_path == nullptr || key_path == nullptr) {
    return nullptr;
  }
  std::ifstream certificate_file(certificate_path);
  std::ifstream key_file(key_path);
  if (!certificate_file || !key_file) {
    return nullptr;
  }
  ::grpc::SslServerCredentialsOptions ssl;
  ssl.pem_key_cert_pairs.push_back({std::string(std::istreambuf_iterator<char>(key_file), {}),
                                    std::string(std::istreambuf_iterator<char>(certificate_file), {})});
  return ::grpc::SslServerCredentials(ssl);
}
core::Result<SutServerConfig> ReadSutServerConfig(int argc, char** argv) {
  const std::string endpoint =
      argc > 1 ? argv[1] : std::string(kDefaultHost).append(":").append(std::to_string(kDefaultPort));
  auto parsed = server::ParseHostPortEndpoint(endpoint, kMaxHttpPort);
  if (!parsed.ok()) {
    return parsed.error();
  }
  SutServerConfig config;
  config.listener = {
      .host = parsed.value().host, .port = parsed.value().port, .grpc_port = parsed.value().port + kGrpcPortOffset};
  const auto& host = config.listener.host;
  config.endpoints = {.rest_url = server::BuildHttpUrl(host, config.listener.port, kRestApiBasePath),
                      .json_rpc_url = server::BuildHttpUrl(host, config.listener.port, kJsonRpcPath),
                      .grpc_url = std::string(host).append(":").append(std::to_string(config.listener.grpc_port))};
  if (const char* public_base = std::getenv(kPublicHttpsBaseEnv)) {
    config.endpoints.rest_url = std::string(public_base).append(kRestApiBasePath);
    config.endpoints.json_rpc_url = std::string(public_base).append(kJsonRpcPath);
  }
  if (const char* public_target = std::getenv(kPublicGrpcTargetEnv)) {
    config.endpoints.grpc_url = public_target;
  }
  return config;
}
server::ServerBindingsOptions BindingOptions() {
  server::ServerBindingsOptions options;
  options.required_extensions = {std::string(kRequiredExtensionUri)};
  options.rest.include_legacy_transport_fields = false;
  options.rest.agent_card_cache_settings = server::RestServerTransportOptions::AgentCardCacheSettings{
      .cache_control = kCacheControl,
      .last_modified = std::chrono::system_clock::from_time_t(kAgentCardLastModifiedUnix)};
  options.jsonrpc.require_version_header = true;
  return options;
}
template <typename HttpListener>
int Serve(SutAgent* agent, const SutServerConfig& config, const SutRuntimeOptions& options,
          server::ServerBindings& bindings, HttpListener& http) {
  SetSignals();
  auto credentials = GrpcCredentials();
  if (!credentials) {
    std::cerr << kInvalidTls << '\n';
    return 1;
  }
  const auto& listener = config.listener;
  if (!http.Start()) {
    return 1;
  }
  ::grpc::ServerBuilder builder;
  builder.AddListeningPort(std::string(listener.host).append(":").append(std::to_string(listener.grpc_port)),
                           credentials);
  builder.RegisterService(&bindings.grpc_service());
  auto grpc = builder.BuildAndStart();
  if (!grpc) {
    return 1;
  }
  http.AcceptConnections(kKeepRunning);
  bindings.Shutdown();
  if (agent != nullptr) {
    agent->ShutdownSubscriptions();
  }
  http.ShutdownActiveSockets();
  http.JoinConnections();
  std::cerr << options.display_name << kHttpJoined;
  if (options.observer != nullptr) {
    options.observer->OnShutdown();
  }
  grpc->Shutdown();
  grpc->Wait();
  return 0;
}

#ifdef A2A_SUT_ASYNC
std::size_t DispatcherThreads(const SutRuntimeOptions& options) {
  return options.dispatcher_threads == 0 ? server::engine::AsyncRuntimeOptions::kDefaultThreads
                                         : options.dispatcher_threads;
}

server::EventHttpServerOptions EventOptions(const SutConfig& listener, const SutRuntimeOptions& options) {
  server::EventHttpServerOptions settings;
  settings.address = listener.host;
  settings.port = static_cast<std::uint16_t>(listener.port);
  if (options.http_pending_limit != 0) {
    settings.max_pending_requests = options.http_pending_limit;
  }
  if (options.http_application_threads != 0) {
    settings.application_threads = options.http_application_threads;
  }
  settings.network_workers = options.http_network_threads;
  return settings;
}
constexpr auto kSignalInterval = std::chrono::milliseconds(10);
void WaitForSignal(const volatile std::sig_atomic_t& keep_running) {
  while (keep_running != 0) {
    std::this_thread::sleep_for(kSignalInterval);
  }
}

class EventSutServer final {
 public:
  EventSutServer(std::shared_ptr<server::engine::AsyncDispatcher> dispatcher, lf::a2a::v1::AgentCard card,
                 const SutConfig& listener, const SutRuntimeOptions& options)
      : server_(std::move(dispatcher), std::move(card), BindingOptions(), EventOptions(listener, options)) {
    if (options.report_topology) {
      const auto settings = EventOptions(listener, options);
      std::cerr << "A2A_PERFORMANCE_TOPOLOGY ingress="
                << (options.ingress == SutIngress::kShared ? "shared" : "separate")
                << " dispatcher_threads=" << DispatcherThreads(options)
                << " http_application_threads=" << settings.application_threads
                << " http_network_threads=" << server_.network_workers()
                << " http_pending_limit=" << settings.max_pending_requests << '\n';
    }
  }
  bool Start() { return server_.Start().ok(); }
  static void AcceptConnections(const volatile std::sig_atomic_t& keep_running) { WaitForSignal(keep_running); }
  void ShutdownActiveSockets() { server_.Shutdown(); }
  void JoinConnections() { server_.Join(); }

 private:
  server::EventHttpServer server_;
};
class ImmediateRead final : public server::engine::CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(
      lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext /*context*/) override {
    lf::a2a::v1::Task task;
    task.set_id(std::move(*request.mutable_id()));
    co_return task;
  }
};

constexpr auto kReadFixtureTask = "benchmark-task";

int ServeReadFixture(server::EventHttpServer& server) {
  SetSignals();
  if (!server.Start().ok()) {
    return 1;
  }
  WaitForSignal(kKeepRunning);
  server.Shutdown();
  server.Join();
  return 0;
}

int RunReadFixture(const SutConfig& listener, const SutRuntimeOptions& options) {
  const auto event_options = EventOptions(listener, options);
  if (options.read_fixture == SutReadFixture::kDispatcher) {
    server::engine::AsyncRuntime engine(std::make_shared<ImmediateRead>());
    server::EventHttpServer server(engine.dispatcher(), {}, BindingOptions(), event_options);
    return ServeReadFixture(server);
  }
  lf::a2a::v1::Task task;
  task.set_id(kReadFixtureTask);
  auto body = core::MessageToJson(task);
  if (!body.ok()) {
    return 1;
  }
  auto response =
      server::HttpServerResponseBuilder()
          .WithStatus(core::http::kStatusOk)
          .WithHeader(core::http::kContentTypeHeaderName, std::string(core::http::kContentTypeApplicationA2aJson))
          .WithA2aVersion()
          .WithActivatedExtensions({std::string(kRequiredExtensionUri)})
          .WithBody(std::move(body.value()))
          .Build();
  server::EventHttpServer server(
      [response = std::move(response)](const server::HttpServerRequest&) { return response; }, event_options);
  return ServeReadFixture(server);
}

template <typename Run>
int WithDispatcher(std::shared_ptr<server::engine::CoroutineAgentExecutor> executor,
                   server::engine::AsyncDispatcherOptions execution, const SutRuntimeOptions& options, Run run) {
  using server::engine::AsyncDispatcher;
  using server::engine::AsyncRuntime;
  const auto threads = DispatcherThreads(options);
  if (options.ingress == SutIngress::kShared) {
    AsyncRuntime engine(std::move(executor), std::move(execution), {.threads = threads});
    return run(engine.dispatcher());
  }
  boost::asio::thread_pool pool(threads);
  auto dispatcher = std::make_shared<AsyncDispatcher>(std::move(executor), pool.get_executor(), std::move(execution));
  const int status = run(dispatcher);
  dispatcher->Shutdown();
  pool.join();
  return status;
}

int RunPipelineFixture(const SutServerConfig& config, const SutRuntimeOptions& options) {
  auto executor = std::make_shared<PipelineReadFixture>();
  auto interceptor = std::make_shared<PipelinePolicy>();
  if (options.asynchronous) {
    server::engine::AsyncDispatcherOptions execution;
    execution.interceptors = {interceptor};
    return WithDispatcher(executor, std::move(execution), options, [&](const auto& dispatcher) {
      server::ServerBindings bindings(dispatcher, {}, BindingOptions());
      EventSutServer http(dispatcher, {}, config.listener, options);
      return Serve(nullptr, config, options, bindings, http);
    });
  }
  server::Dispatcher dispatcher(executor.get(), {interceptor});
  server::ServerBindings bindings(dispatcher, {}, BindingOptions());
  TckHttpServer http(config.listener.host, config.listener.port, bindings.http_mux(), nullptr);
  return Serve(nullptr, config, options, bindings, http);
}

int RunAsync(SutAgent& agent, const SutServerConfig& config, const SutRuntimeOptions& options) {
  server::engine::AsyncDispatcherOptions execution;
  execution.agent_card_provider = agent.card_provider();
  execution.interceptors = {agent.interceptor()};
  execution.security_policy = agent.security_policy();
  execution.task_store = agent.task_store();
  return WithDispatcher(std::make_shared<SutCoroutineExecutor>(agent, options.get_task_delay), std::move(execution),
                        options, [&](const auto& dispatcher) {
                          server::ServerBindings bindings(dispatcher, agent.agent_card(), BindingOptions());
                          EventSutServer http(dispatcher, agent.agent_card(), config.listener, options);
                          return Serve(&agent, config, options, bindings, http);
                        });
}
#endif
}  // namespace
int RunSutRuntime(int argc, char** argv, const SutRuntimeOptions& options) {
  auto config = ReadSutServerConfig(argc, argv);
#ifdef A2A_SUT_ASYNC
  if (config.ok() && options.read_fixture == SutReadFixture::kPipeline) {
    return RunPipelineFixture(config.value(), options);
  }
  if (config.ok() && options.read_fixture != SutReadFixture::kDisabled) {
    return RunReadFixture(config.value().listener, options);
  }
#endif
  auto agent_options = ReadSutAgentOptions(options.asynchronous);
  auto stores = CreateStoreBundleFromEnvironment();
  if (!config.ok() || !agent_options.ok() || !stores.ok()) {
    std::cerr << options.display_name << " configuration failed\n";
    if (!agent_options.ok()) {
      std::cerr << agent_options.error().message() << '\n';
    }
    return 1;
  }
  agent_options.value().allow_opaque_input = options.allow_opaque_input;
  if (!options.asynchronous) {
    agent_options.value().blocking_get_task_delay = options.get_task_delay;
  }
  SutAgent agent(std::move(stores.value()), config.value().endpoints, std::move(agent_options.value()));
#ifdef A2A_SUT_ASYNC
  if (options.seed_pipeline_task && !SeedPipelineTask(*agent.task_store()).ok()) {
    std::cerr << kPipelineStoreSetupFailed << '\n';
    return 1;
  }
  if (options.asynchronous) {
    return RunAsync(agent, config.value(), options);
  }
#endif
  server::ServerBindings bindings(agent.dispatcher(), agent.agent_card(), BindingOptions());
  TckHttpServer http(config.value().listener.host, config.value().listener.port, bindings.http_mux(), options.observer);
  return Serve(&agent, config.value(), options, bindings, http);
}
}  // namespace a2a::tests::sut
