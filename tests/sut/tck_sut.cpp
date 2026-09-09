// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/agent_card/agent_card_provider.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif
#include "a2a/server/dispatcher.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/network_utils.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/transport_mux.h"
#include "example_support.h"
#include "sut/tck_sut.h"
#include "sut/tck_sut_http_server.h"
#include "sut/tck_sut_store.h"

namespace {

using namespace a2a::tests::sut;

constexpr int kMaxHttpPort = 65534;
constexpr std::time_t kAgentCardLastModifiedUnix = 1704067200;
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
constexpr std::string_view kSubscriptionDiagnosticsPrefix = "A2A_SUBSCRIPTION_SERVER_DIAGNOSTICS";
#endif
volatile std::sig_atomic_t kKeepRunning = 1;

#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
void EmitSubscriptionDiagnostics() {
  if (!a2a::core::subscription_diagnostics::IsEnabled()) {
    return;
  }
  const auto snapshot = a2a::core::subscription_diagnostics::TakeSnapshot();
  std::cout << kSubscriptionDiagnosticsPrefix;
  for (std::size_t index = 0; index < snapshot.size(); ++index) {
    const auto& aggregate = snapshot[index];
    const auto phase_name = a2a::core::subscription_diagnostics::kPhaseNames[index];
    std::cout << ' ' << phase_name << "_count=" << aggregate.count << ' ' << phase_name
              << "_total_ns=" << aggregate.elapsed_nanoseconds << ' ' << phase_name
              << "_max_ns=" << aggregate.maximum_nanoseconds;
  }
  std::cout << '\n' << std::flush;
}
#endif

void SignalHandler(int signal_number) {
  (void)signal_number;
  kKeepRunning = 0;
}

int RunTckSut(int argc, char** argv) {
  const std::string endpoint = (argc > 1) ? argv[1] : std::string(kDefaultHost) + ":" + std::to_string(kDefaultPort);
  auto parsed_endpoint = a2a::server::ParseHostPortEndpoint(endpoint, kMaxHttpPort);
  if (!parsed_endpoint.ok()) {
    std::cerr << parsed_endpoint.error().message() << '\n';
    return 1;
  }
  const std::string& host = parsed_endpoint.value().host;
  const int port = parsed_endpoint.value().port;
  const int grpc_port = port + kGrpcPortOffset;
  const SutEndpoints endpoints{.rest_url = a2a::server::BuildHttpUrl(host, port, kRestApiBasePath),
                               .json_rpc_url = a2a::server::BuildHttpUrl(host, port, kJsonRpcPath),
                               .grpc_url = host + ":" + std::to_string(grpc_port)};

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);
#ifndef _WIN32
  std::signal(SIGPIPE, SIG_IGN);
#endif
#ifdef _WIN32
  std::signal(SIGBREAK, SignalHandler);
#endif

  const char* extended_card_mode_value = std::getenv(kExtendedCardModeEnv);
  const std::string_view extended_card_mode =
      extended_card_mode_value == nullptr ? kExtendedCardModeConfigured : std::string_view(extended_card_mode_value);
  if (extended_card_mode != kExtendedCardModeConfigured && extended_card_mode != kExtendedCardModeDeclaredOnly &&
      extended_card_mode != kExtendedCardModeDisabled) {
    std::cerr << "Unsupported A2A_TCK_EXTENDED_AGENT_CARD_MODE: " << extended_card_mode << '\n';
    return 1;
  }
  const bool declares_extended_card = extended_card_mode != kExtendedCardModeDisabled;
  const bool configures_extended_card = extended_card_mode == kExtendedCardModeConfigured;

  auto agent_card =
      a2a::core::AgentCardBuilder::ConformancePreset(
          {.rest_url = endpoints.rest_url, .json_rpc_url = endpoints.json_rpc_url, .grpc_url = endpoints.grpc_url},
          "TCK SUT", "0.1.0", "Conformance-focused local SUT for A2A")
          .WithPushNotifications(true)
          .WithExtendedAgentCard(declares_extended_card)
          .Build();

  std::optional<lf::a2a::v1::AgentCard> extended_agent_card;
  if (configures_extended_card) {
    extended_agent_card = agent_card;
    extended_agent_card->set_description("Extended conformance-focused local SUT card for A2A");
  }

  auto store_bundle = CreateStoreBundleFromEnvironment();
  if (!store_bundle.ok()) {
    std::cerr << "Failed to create TCK SUT store bundle: " << store_bundle.error().message() << '\n';
    return 1;
  }

  a2a::examples::ExampleExecutorOptions executor_options;
  executor_options.task_store = store_bundle.value().task_store.get();
  executor_options.push_store = store_bundle.value().push_store.get();
  a2a::examples::ExampleExecutor executor(std::move(executor_options));
  auto agent_card_provider = std::make_shared<a2a::core::StaticAgentCardProvider>(extended_agent_card);
  a2a::server::Dispatcher dispatcher(&executor, agent_card_provider);
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
  a2a::server::RestServerTransport rest(&dispatcher, agent_card, std::move(rest_options));

  a2a::server::JsonRpcServerTransportOptions jsonrpc_options;
  jsonrpc_options.rpc_path = std::string(kJsonRpcPath);
  jsonrpc_options.require_version_header = false;
  jsonrpc_options.required_extensions = {std::string(kRequiredExtensionUri)};
  a2a::server::JsonRpcServerTransport jsonrpc(&dispatcher, std::move(jsonrpc_options));

  a2a::server::TransportMux mux(
      {.normalization_policy = a2a::server::TransportMux::PathNormalizationPolicy::kRootToDefaultPath,
       .default_path = std::string(kJsonRpcPath)});
  mux.RegisterJsonRpcRoute(jsonrpc);
  mux.RegisterRestRoute(rest);

  TckHttpServer http_server(host, port, mux);
  if (!http_server.Start()) {
    return 1;
  }

  grpc::ServerBuilder grpc_builder;
  grpc_builder.AddListeningPort(host + ":" + std::to_string(grpc_port), grpc::InsecureServerCredentials());
  grpc_builder.RegisterService(&grpc);
  std::unique_ptr<grpc::Server> grpc_server = grpc_builder.BuildAndStart();
  if (!grpc_server) {
    std::cerr << "Failed to start TCK SUT gRPC server on " << host << ':' << grpc_port << '\n';
    return 1;
  }

  http_server.AcceptConnections(kKeepRunning);
  std::cerr << "TCK SUT shutdown: stopping subscriptions\n";
  executor.ShutdownSubscriptions();
  std::cerr << "TCK SUT shutdown: shutting down active HTTP sockets\n";
  http_server.ShutdownActiveSockets();
  std::cerr << "TCK SUT shutdown: joining HTTP connection threads\n";
  http_server.JoinConnections();
  std::cerr << "TCK SUT shutdown: HTTP connection threads joined\n";
  http_server.EmitDiagnostics();
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  EmitSubscriptionDiagnostics();
#endif
  std::cerr << "TCK SUT shutdown: stopping gRPC\n";
  grpc_server->Shutdown();
  return 0;
}

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    return RunTckSut(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled TCK SUT exception: " << ex.what() << '\n';
    return 1;
  }
}
