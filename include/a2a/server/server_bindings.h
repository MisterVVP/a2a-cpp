// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "a2a/core/non_copyable.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/transport_mux.h"

namespace a2a::server {
namespace engine {
class AsyncDispatcher;
}
struct ServerBindingsOptions final {
  static constexpr auto kDefaultRestPath = "/a2a";
  static constexpr auto kDefaultJsonRpcPath = "/rpc";
  // A nonempty list overrides requirements consistently on all enabled bindings.
  // An empty list preserves the individual transport options below.
  std::vector<std::string> required_extensions;
  GrpcServerTransportOptions grpc;
  RestServerTransportOptions rest = {.rest_api_base_path = kDefaultRestPath, .agent_card_cache_settings = {}};
  JsonRpcServerTransportOptions jsonrpc = {.rpc_path = kDefaultJsonRpcPath};
};

// Owns transport registrations, not listeners. Register grpc_service() with your
// gRPC host and send HTTP requests to http_mux(). Keep the host within this
// object's lifetime. The synchronous dispatcher must outlive the bindings.
class ServerBindings final : private core::NonCopyableOrMovable {
 public:
  // All three synchronous transports; HTTP root aliases jsonrpc.rpc_path.
  ServerBindings(Dispatcher& dispatcher, lf::a2a::v1::AgentCard card, ServerBindingsOptions options = {});
  // Requires a2a::asio. Native async gRPC plus HTTP agent-card discovery only;
  // jsonrpc options are unused. Does not register synchronous operation routes.
  ServerBindings(std::shared_ptr<engine::AsyncDispatcher> dispatcher, lf::a2a::v1::AgentCard card,
                 ServerBindingsOptions options = {});
  ~ServerBindings();
  [[nodiscard]] ::grpc::Service& grpc_service();
  [[nodiscard]] const TransportMux& http_mux() const;
  // Close async service admission before stopping the listener. No-op for sync
  // execution; the application owns its synchronous subscription shutdown.
  void Shutdown();

 private:
  struct State;
  std::unique_ptr<State> state_;
};
}  // namespace a2a::server
