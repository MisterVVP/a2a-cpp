// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <memory>
#include <utility>

#include "a2a/server/async_grpc_server_transport.h"
#include "a2a/server/server_bindings.h"
#include "server_bindings_internal.h"

namespace a2a::server {
ServerBindings::ServerBindings(std::shared_ptr<engine::AsyncDispatcher> dispatcher, lf::a2a::v1::AgentCard card,
                               ServerBindingsOptions options)
    : state_(std::make_unique<State>(nullptr, std::move(card), options)) {
  if (!options.required_extensions.empty()) {
    options.grpc.required_extensions = options.required_extensions;
  }
  auto transport = std::make_unique<AsyncGrpcServerTransport>(std::move(dispatcher), std::move(options.grpc));
  state_->shutdown = [service = transport.get()] { service->Shutdown(); };
  state_->grpc = std::move(transport);
  state_->mux.RegisterAgentCardRoute(state_->rest);
}
}  // namespace a2a::server
