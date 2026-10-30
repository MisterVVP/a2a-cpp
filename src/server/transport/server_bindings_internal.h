// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "a2a/server/server_bindings.h"

namespace a2a::server {
struct ServerBindings::State final {
  State(Dispatcher* dispatcher, lf::a2a::v1::AgentCard card, ServerBindingsOptions options)
      : rest(dispatcher, std::move(card), RestOptions(options)), mux(RoutingOptions(dispatcher, options)) {}
  static TransportMux::Options RoutingOptions(Dispatcher* dispatcher, const ServerBindingsOptions& options) {
    if (dispatcher == nullptr) {
      return {.normalization_policy = TransportMux::PathNormalizationPolicy::kTrimTrailingSlash, .default_path = {}};
    }
    return {.normalization_policy = TransportMux::PathNormalizationPolicy::kRootToDefaultPath,
            .default_path = options.jsonrpc.rpc_path};
  }
  static RestServerTransportOptions RestOptions(const ServerBindingsOptions& options) {
    auto rest = options.rest;
    if (!options.required_extensions.empty()) {
      rest.required_extensions = options.required_extensions;
    }
    return rest;
  }
  RestServerTransport rest;
  std::unique_ptr<JsonRpcServerTransport> jsonrpc;
  TransportMux mux;
  std::unique_ptr<::grpc::Service> grpc;
  std::function<void()> shutdown;
};
}  // namespace a2a::server
