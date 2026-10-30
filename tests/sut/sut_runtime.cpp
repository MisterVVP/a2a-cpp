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
#include <utility>

#include "a2a/server/network_utils.h"
#include "a2a/server/server_bindings.h"
#include "sut/sut_agent.h"
#include "sut/tck_sut_http_server.h"
#include "sut/tck_sut_store.h"
#ifdef A2A_SUT_ASYNC
#include "a2a/server/engine/async_runtime.h"
#include "a2a/server/engine/blocking_executor_adapter.h"
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
int Serve(SutAgent& agent, const SutServerConfig& config, const SutRuntimeOptions& options,
          server::ServerBindings& bindings) {
  SetSignals();
  auto credentials = GrpcCredentials();
  if (!credentials) {
    std::cerr << kInvalidTls << '\n';
    return 1;
  }
  const auto& listener = config.listener;
  TckHttpServer http(listener.host, listener.port, bindings.http_mux(), options.observer);
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
  agent.ShutdownSubscriptions();
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
int RunAsync(SutAgent& agent, const SutServerConfig& config, const SutRuntimeOptions& options) {
  server::engine::AsyncDispatcherOptions execution;
  execution.agent_card_provider = agent.card_provider();
  execution.interceptors = {agent.interceptor()};
  execution.security_policy = agent.security_policy();
  execution.task_store = agent.task_store();
  server::engine::AsyncRuntime engine(std::make_shared<server::engine::BlockingExecutorAdapter>(agent.executor()),
                                      std::move(execution));
  server::ServerBindings bindings(engine.dispatcher(), agent.agent_card(), BindingOptions());
  return Serve(agent, config, options, bindings);
}
#endif
}  // namespace
int RunSutRuntime(int argc, char** argv, const SutRuntimeOptions& options) {
  auto config = ReadSutServerConfig(argc, argv);
  auto agent_options = ReadSutAgentOptions(options.asynchronous);
  auto stores = CreateStoreBundleFromEnvironment();
  if (!config.ok() || !agent_options.ok() || !stores.ok()) {
    std::cerr << options.display_name << " configuration failed\n";
    if (!agent_options.ok()) {
      std::cerr << agent_options.error().message() << '\n';
    }
    return 1;
  }
  SutAgent agent(std::move(stores.value()), config.value().endpoints, std::move(agent_options.value()));
#ifdef A2A_SUT_ASYNC
  if (options.asynchronous) {
    return RunAsync(agent, config.value(), options);
  }
#endif
  server::ServerBindings bindings(agent.dispatcher(), agent.agent_card(), BindingOptions());
  return Serve(agent, config.value(), options, bindings);
}
}  // namespace a2a::tests::sut
