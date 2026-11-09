// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#pragma once
#include <memory>

#include "a2a/server/engine/async_dispatcher.h"
#include "a2a/server/event_http_server.h"
#include "a2a/server/server_bindings.h"

namespace a2a::server {
// Owns protocol transports and a dispatcher reference. Reuses the synchronous
// bindings' request validation, routing and response/event serializers. Native
// unary calls and finite/live streams suspend without occupying a worker.
[[nodiscard]] EventHttpServer::AsyncHandler MakeEventHttpHandler(std::shared_ptr<engine::AsyncDispatcher> dispatcher,
                                                                 lf::a2a::v1::AgentCard card,
                                                                 const ServerBindingsOptions& options = {});
}  // namespace a2a::server
