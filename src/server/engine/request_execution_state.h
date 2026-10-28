// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <optional>
#include <stop_token>
#include <utility>

#include "a2a/server/engine/request_execution.h"

namespace a2a::server::engine {

struct RequestExecutionState final {
  explicit RequestExecutionState(RequestContext request_metadata,
                                 std::optional<RequestExecutionContext::Clock::time_point> request_deadline)
      : metadata(std::move(request_metadata)), deadline(request_deadline) {}

  [[nodiscard]] bool TryFinish(RequestOutcome terminal) noexcept {
    auto expected = RequestOutcome::kPending;
    if (!outcome.compare_exchange_strong(expected, terminal)) {
      return false;
    }
    if (terminal != RequestOutcome::kSucceeded && terminal != RequestOutcome::kFailed) {
      // Publish the reason before notifying observers. No SDK lock is held while
      // callbacks run, so observers may inspect state or attempt another signal.
      stop_source.request_stop();
    }
    return true;
  }

  const RequestContext metadata;
  const std::optional<RequestExecutionContext::Clock::time_point> deadline;
  std::atomic<RequestOutcome> outcome{RequestOutcome::kPending};
  std::stop_source stop_source;
};

}  // namespace a2a::server::engine
