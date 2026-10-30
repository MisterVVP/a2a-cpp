// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/server_bindings.h"

#include <memory>
#include <utility>

#include "server_bindings_internal.h"

namespace a2a::server {
ServerBindings::ServerBindings(Dispatcher& dispatcher, lf::a2a::v1::AgentCard card, ServerBindingsOptions options)
    : state_(std::make_unique<State>(&dispatcher, std::move(card), options)) {
  if (!options.required_extensions.empty()) {
    options.grpc.required_extensions = options.required_extensions;
    options.jsonrpc.required_extensions = options.required_extensions;
  }
  state_->grpc = std::make_unique<GrpcServerTransport>(&dispatcher, std::move(options.grpc));
  TransportMux::JsonRpcRouteOptions rpc_route;
  rpc_route.rpc_path = options.jsonrpc.rpc_path;
  state_->jsonrpc = std::make_unique<JsonRpcServerTransport>(&dispatcher, std::move(options.jsonrpc));
  state_->mux.RegisterJsonRpcRoute(*state_->jsonrpc, std::move(rpc_route));
  TransportMux::RestRouteOptions rest_route;
  rest_route.rest_api_prefix = options.rest.rest_api_base_path;
  state_->mux.RegisterRestRoute(state_->rest, std::move(rest_route));
}
ServerBindings::~ServerBindings() { Shutdown(); }
::grpc::Service& ServerBindings::grpc_service() { return *state_->grpc; }
const TransportMux& ServerBindings::http_mux() const { return state_->mux; }
void ServerBindings::Shutdown() {
  if (state_->shutdown) {
    state_->shutdown();
  }
}
}  // namespace a2a::server
