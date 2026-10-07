// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
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
  virtual void Cancel() noexcept {}
};

}  // namespace a2a::server
