// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/async_sut_runtime.h"

#include <cstdlib>
#include <iostream>
#include <utility>

#include "a2a/server/async_grpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/transport_mux.h"
#include "sut/async_sut_agent.h"
#include "sut/sut_server_runtime.h"
#include "sut/tck_sut_store.h"

namespace a2a::tests::sut {
namespace {
constexpr auto kAsyncSutName = "Async TCK SUT";
constexpr auto kDiscoveryRoute = "agent-card-discovery";
constexpr auto kUnsupportedTransports =
    "Only grpc is implemented in the async SUT; jsonrpc and http_json are not enabled yet";
core::Result<SutAgentOptions> ReadAsyncOptions() {
  auto options = ReadSutAgentOptions();
  if (!options.ok()) {
    return options.error();
  }
  const auto* names = std::getenv(kSutTransportsEnv);
  auto transports = ParseSutTransports(names == nullptr ? kGrpcTransport : std::string_view(names));
  if (!transports.ok()) {
    return transports.error();
  }
  if (!transports.value().grpc || transports.value().jsonrpc || transports.value().http_json) {
    return core::Error::Validation(kUnsupportedTransports);
  }
  options.value().transports = transports.value();
  options.value().streaming = false;
  return options;
}
}  // namespace
int RunAsyncSutRuntime(int argc, char** argv) {
  auto config = ReadSutServerConfig(argc, argv);
  auto options = ReadAsyncOptions();
  auto stores = CreateStoreBundleFromEnvironment();
  if (!config.ok() || !options.ok() || !stores.ok()) {
    std::cerr << kAsyncSutName << " configuration failed\n";
    if (!options.ok()) {
      std::cerr << options.error().message() << '\n';
    }
    return 1;
  }
  AsyncSutAgent agent(std::move(stores.value()), config.value().endpoints, std::move(options.value()));
  server::GrpcServerTransportOptions grpc_options;
  grpc_options.required_extensions = {std::string(kRequiredExtensionUri)};
  server::AsyncGrpcServerTransport grpc(agent.dispatcher(), std::move(grpc_options));
  server::RestServerTransport discovery(nullptr, agent.agent_card(), SutRestOptions());
  server::TransportMux mux;
  mux.RegisterRoute({.name = kDiscoveryRoute,
                     .matcher =
                         [](std::string_view, std::string_view path) {
                           return path == server::RestServerTransport::kAgentCardPath ||
                                  path == server::RestServerTransport::kLegacyAgentCardPath;
                         },
                     .handler = [&discovery](const auto& request) { return discovery.Handle(request); }});
  SutServerRuntime runtime(std::move(config.value()), {.display_name = kAsyncSutName});
  if (!runtime.Start(mux, grpc)) {
    return 1;
  }
  runtime.Run();
  grpc.Shutdown();
  agent.Shutdown();
  runtime.ShutdownHttp();
  runtime.ShutdownGrpc();
  return 0;
}
}  // namespace a2a::tests::sut
