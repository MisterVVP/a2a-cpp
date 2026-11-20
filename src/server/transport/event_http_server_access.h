// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "a2a/server/event_http_server.h"

namespace a2a::server::http_runtime {

class EventHttpServerAccess final {
 public:
  static EventHttpServer MakeNative(std::shared_ptr<engine::AsyncDispatcher> dispatcher, lf::a2a::v1::AgentCard card,
                                    const ServerBindingsOptions& bindings, EventHttpServerOptions options,
                                    std::size_t network_workers) {
    return EventHttpServer(std::move(dispatcher), std::move(card), bindings, std::move(options), network_workers);
  }

  static std::size_t NetworkWorkers(const EventHttpServer& server) noexcept;
};

}  // namespace a2a::server::http_runtime
