// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>

#include "a2a/core/non_copyable.h"
#include "a2a/server/request_context.h"

namespace a2a::server::engine {

struct RequestExecutionState;

enum class RequestOutcome { kPending, kSucceeded, kFailed, kClientCancelled, kDeadlineExceeded, kServerShutdown };
enum class RequestCompletion { kSucceeded, kFailed };
enum class RequestCancellationReason { kClientCancelled, kDeadlineExceeded, kServerShutdown };

// Copyable application view. Owns the immutable metadata and shared lifecycle state;
// it never borrows transport objects. Concurrent reads of separate copies are safe.
class RequestExecutionContext final {
 public:
  using Clock = std::chrono::steady_clock;

  [[nodiscard]] const RequestContext& Metadata() const noexcept;
  [[nodiscard]] std::optional<Clock::time_point> Deadline() const noexcept;
  [[nodiscard]] RequestOutcome Outcome() const noexcept;
  [[nodiscard]] std::stop_token StopToken() const noexcept;

 private:
  friend class RequestExecution;
  explicit RequestExecutionContext(std::shared_ptr<RequestExecutionState> state) noexcept;
  std::shared_ptr<RequestExecutionState> state_;
};

// Single owner for one operation. Concurrent terminal signals are safe while this
// object is alive. Destruction publishes shutdown if the operation is still pending.
// No timer or worker is started: the caller drives ExpireDeadline from its scheduler.
class RequestExecution final : private core::NonCopyableOrMovable {
 public:
  using Clock = RequestExecutionContext::Clock;

  explicit RequestExecution(RequestContext metadata = {}, std::optional<Clock::time_point> deadline = {});
  ~RequestExecution();
  [[nodiscard]] RequestExecutionContext Context() const noexcept;
  // Returns true only for the winning application completion. At/after the
  // deadline, publishes deadline expiry instead and returns false.
  [[nodiscard]] bool TryComplete(RequestCompletion completion, Clock::time_point now = Clock::now()) noexcept;
  // First terminal transition wins. A transport may report its own deadline signal.
  [[nodiscard]] bool Cancel(RequestCancellationReason reason = RequestCancellationReason::kClientCancelled) noexcept;
  [[nodiscard]] bool ExpireDeadline(Clock::time_point now = Clock::now()) noexcept;

 private:
  std::shared_ptr<RequestExecutionState> state_;
};

}  // namespace a2a::server::engine
