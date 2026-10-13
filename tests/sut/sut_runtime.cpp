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
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/server/dispatcher.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/network_utils.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/transport_mux.h"
#include "sut/sut_agent.h"
#include "sut/tck_sut.h"
#include "sut/tck_sut_http_server.h"
#include "sut/tck_sut_store.h"

namespace {

using namespace a2a::tests::sut;

constexpr int kMaxHttpPort = 65534;
constexpr std::time_t kAgentCardLastModifiedUnix = 1704067200;
volatile std::sig_atomic_t kKeepRunning = 1;

void SignalHandler(int signal_number) {
  (void)signal_number;
  kKeepRunning = 0;
}

int RunSutRuntimeImpl(int argc, char** argv, const SutRuntimeOptions& options) {
  const std::string endpoint = (argc > 1) ? argv[1] : std::string(kDefaultHost) + ":" + std::to_string(kDefaultPort);
  auto parsed_endpoint = a2a::server::ParseHostPortEndpoint(endpoint, kMaxHttpPort);
  if (!parsed_endpoint.ok()) {
    std::cerr << parsed_endpoint.error().message() << '\n';
    return 1;
  }
  const std::string& host = parsed_endpoint.value().host;
  const int port = parsed_endpoint.value().port;
  const int grpc_port = port + kGrpcPortOffset;
  SutEndpoints endpoints{.rest_url = a2a::server::BuildHttpUrl(host, port, kRestApiBasePath),
                         .json_rpc_url = a2a::server::BuildHttpUrl(host, port, kJsonRpcPath),
                         .grpc_url = host + ":" + std::to_string(grpc_port)};

  if (const char* public_base = std::getenv(kPublicHttpsBaseEnv)) {
    endpoints.rest_url = std::string(public_base).append(kRestApiBasePath);
    endpoints.json_rpc_url = std::string(public_base).append(kJsonRpcPath);
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
#ifdef _WIN32
  std::signal(SIGBREAK, SignalHandler);
#endif

  const auto agent_options = ReadSutAgentOptions();
  if (!agent_options.ok()) {
    std::cerr << agent_options.error().message() << '\n';
    return 1;
  }

  auto store_bundle = CreateStoreBundleFromEnvironment();
  if (!store_bundle.ok()) {
    std::cerr << "Failed to create " << options.display_name << " store bundle: " << store_bundle.error().message()
              << '\n';
    return 1;
  }

  SutAgent agent(std::move(store_bundle.value()), endpoints, agent_options.value());
  auto& dispatcher = agent.dispatcher();
  a2a::server::GrpcServerTransportOptions grpc_options;
  grpc_options.required_extensions = {std::string(kRequiredExtensionUri)};
  a2a::server::GrpcServerTransport grpc(&dispatcher, std::move(grpc_options));

  a2a::server::RestServerTransportOptions rest_options;
  rest_options.rest_api_base_path = std::string(kRestApiBasePath);
  rest_options.include_legacy_transport_fields = false;
  rest_options.required_extensions = {std::string(kRequiredExtensionUri)};
  rest_options.agent_card_cache_settings = a2a::server::RestServerTransportOptions::AgentCardCacheSettings{
      .cache_control = "public, max-age=300",
      .last_modified = std::chrono::system_clock::from_time_t(kAgentCardLastModifiedUnix)};
  a2a::server::RestServerTransport rest(&dispatcher, agent.agent_card(), std::move(rest_options));

  a2a::server::JsonRpcServerTransportOptions jsonrpc_options;
  jsonrpc_options.rpc_path = std::string(kJsonRpcPath);
  jsonrpc_options.require_version_header = true;
  jsonrpc_options.required_extensions = {std::string(kRequiredExtensionUri)};
  a2a::server::JsonRpcServerTransport jsonrpc(&dispatcher, std::move(jsonrpc_options));

  a2a::server::TransportMux mux(
      {.normalization_policy = a2a::server::TransportMux::PathNormalizationPolicy::kRootToDefaultPath,
       .default_path = std::string(kJsonRpcPath)});
  mux.RegisterJsonRpcRoute(jsonrpc);
  mux.RegisterRestRoute(rest);

  TckHttpServer http_server(host, port, mux, options.observer);
  if (!http_server.Start()) {
    return 1;
  }

  grpc::ServerBuilder grpc_builder;
  std::shared_ptr<grpc::ServerCredentials> credentials = grpc::InsecureServerCredentials();
  const char* certificate_path = std::getenv(kTlsCertificateFileEnv);
  const char* key_path = std::getenv(kTlsKeyFileEnv);
  if (certificate_path != nullptr || key_path != nullptr) {
    if (certificate_path == nullptr || key_path == nullptr) {
      return 1;
    }
    std::ifstream certificate_file(certificate_path);
    std::ifstream key_file(key_path);
    grpc::SslServerCredentialsOptions ssl;
    ssl.pem_key_cert_pairs.push_back({std::string(std::istreambuf_iterator<char>(key_file), {}),
                                      std::string(std::istreambuf_iterator<char>(certificate_file), {})});
    credentials = grpc::SslServerCredentials(ssl);
  }
  grpc_builder.AddListeningPort(host + ":" + std::to_string(grpc_port), credentials);
  grpc_builder.RegisterService(&grpc);
  std::unique_ptr<grpc::Server> grpc_server = grpc_builder.BuildAndStart();
  if (!grpc_server) {
    std::cerr << "Failed to start " << options.display_name << " gRPC server on " << host << ':' << grpc_port << '\n';
    return 1;
  }

  http_server.AcceptConnections(kKeepRunning);
  std::cerr << options.display_name << " shutdown: stopping subscriptions\n";
  agent.ShutdownSubscriptions();
  std::cerr << options.display_name << " shutdown: shutting down active HTTP sockets\n";
  http_server.ShutdownActiveSockets();
  std::cerr << options.display_name << " shutdown: joining HTTP connection threads\n";
  http_server.JoinConnections();
  std::cerr << options.display_name << " shutdown: HTTP connection threads joined\n";
  if (options.observer != nullptr) {
    options.observer->OnShutdown();
  }
  std::cerr << options.display_name << " shutdown: stopping gRPC\n";
  grpc_server->Shutdown();
  return 0;
}

}  // namespace

namespace a2a::tests::sut {

int RunSutRuntime(int argc, char** argv, const SutRuntimeOptions& options) {
  return RunSutRuntimeImpl(argc, argv, options);
}

}  // namespace a2a::tests::sut
