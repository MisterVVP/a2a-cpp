// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class ServerStreamSession {
 public:
  virtual ~ServerStreamSession() = default;

  [[nodiscard]] virtual core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() = 0;
  [[nodiscard]] virtual core::Result<std::optional<lf::a2a::v1::StreamResponse>> NextFor(
      std::chrono::milliseconds timeout) {
    (void)timeout;
    return Next();
  }
  // Timed reads let transports observe client cancellation without calling
  // Cancel() concurrently with a blocking Next().
  [[nodiscard]] virtual bool SupportsTimedNext() const noexcept { return false; }
  // Sessions are finite unless they explicitly support waiting for future events.
  [[nodiscard]] virtual bool IsLive() const noexcept { return false; }
  // Opt-in readiness for non-blocking NextFor(0). Notifications may be
  // coalesced and run on a publisher thread. The callback only schedules work.
  // Installing it must notify already-ready events; clearing it prevents new
  // invocations after return. Live sessions without this capability retain
  // their existing blocking transport behavior.
  [[nodiscard]] virtual bool SetReadyCallback(std::function<void()> callback) {
    (void)callback;
    return false;
  }
  // Optional producer queue budget for asynchronous callers.
  virtual void SetPendingEventByteLimit(std::size_t limit) { (void)limit; }
  virtual void Cancel() noexcept {}
};

}  // namespace a2a::server
