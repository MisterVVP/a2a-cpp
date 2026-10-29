// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/sut_runtime.h"

#include <iostream>
#include <utility>

#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/transport_mux.h"
#include "sut/sut_agent.h"
#include "sut/sut_server_runtime.h"
#include "sut/tck_sut_store.h"

namespace a2a::tests::sut {
int RunSutRuntime(int argc, char** argv, const SutRuntimeOptions& options) {
  auto config = ReadSutServerConfig(argc, argv);
  auto agent_options = ReadSutAgentOptions();
  auto stores = CreateStoreBundleFromEnvironment();
  if (!config.ok() || !agent_options.ok() || !stores.ok()) {
    std::cerr << options.display_name << " configuration failed\n";
    return 1;
  }
  SutAgent agent(std::move(stores.value()), config.value().endpoints, std::move(agent_options.value()));
  auto& dispatcher = agent.dispatcher();
  server::GrpcServerTransportOptions grpc_options;
  grpc_options.required_extensions = {std::string(kRequiredExtensionUri)};
  server::GrpcServerTransport grpc(&dispatcher, std::move(grpc_options));
  server::RestServerTransport rest(&dispatcher, agent.agent_card(), SutRestOptions());
  server::JsonRpcServerTransportOptions jsonrpc_options;
  jsonrpc_options.rpc_path = std::string(kJsonRpcPath);
  jsonrpc_options.require_version_header = true;
  jsonrpc_options.required_extensions = {std::string(kRequiredExtensionUri)};
  server::JsonRpcServerTransport jsonrpc(&dispatcher, std::move(jsonrpc_options));
  server::TransportMux mux({.normalization_policy = server::TransportMux::PathNormalizationPolicy::kRootToDefaultPath,
                            .default_path = std::string(kJsonRpcPath)});
  mux.RegisterJsonRpcRoute(jsonrpc);
  mux.RegisterRestRoute(rest);
  SutServerRuntime runtime(std::move(config.value()), options);
  if (!runtime.Start(mux, grpc)) {
    return 1;
  }
  runtime.Run();
  agent.ShutdownSubscriptions();
  runtime.ShutdownHttp();
  runtime.ShutdownGrpc();
  return 0;
}
}  // namespace a2a::tests::sut
