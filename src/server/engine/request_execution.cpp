// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/request_execution.h"

#include <utility>

#include "server/engine/request_execution_state.h"

namespace a2a::server::engine {

RequestExecutionContext::RequestExecutionContext(std::shared_ptr<RequestExecutionState> state) noexcept
    : state_(std::move(state)) {}

const RequestContext& RequestExecutionContext::Metadata() const noexcept { return state_->metadata; }

std::optional<RequestExecutionContext::Clock::time_point> RequestExecutionContext::Deadline() const noexcept {
  return state_->deadline;
}

RequestOutcome RequestExecutionContext::Outcome() const noexcept { return state_->outcome.load(); }

std::stop_token RequestExecutionContext::StopToken() const noexcept { return state_->stop_source.get_token(); }

RequestExecution::RequestExecution(RequestContext metadata, std::optional<Clock::time_point> deadline)
    : state_(std::make_shared<RequestExecutionState>(std::move(metadata), deadline)) {}

RequestExecution::~RequestExecution() { (void)Cancel(RequestCancellationReason::kServerShutdown); }

RequestExecutionContext RequestExecution::Context() const noexcept { return RequestExecutionContext(state_); }

bool RequestExecution::TryComplete(RequestCompletion completion, Clock::time_point now) noexcept {
  const auto state = state_;
  const auto deadline = state->deadline;
  if (deadline && now >= *deadline) {
    (void)state->TryFinish(RequestOutcome::kDeadlineExceeded);
    return false;
  }
  return state->TryFinish(completion == RequestCompletion::kSucceeded ? RequestOutcome::kSucceeded
                                                                      : RequestOutcome::kFailed);
}

bool RequestExecution::Cancel(RequestCancellationReason reason) noexcept {
  // Keep state alive even if an inline observer releases the operation owner.
  const auto state = state_;
  switch (reason) {
    case RequestCancellationReason::kClientCancelled:
      return state->TryFinish(RequestOutcome::kClientCancelled);
    case RequestCancellationReason::kDeadlineExceeded:
      return state->TryFinish(RequestOutcome::kDeadlineExceeded);
    case RequestCancellationReason::kServerShutdown:
      return state->TryFinish(RequestOutcome::kServerShutdown);
  }
  return false;
}

bool RequestExecution::ExpireDeadline(Clock::time_point now) noexcept {
  const auto state = state_;
  const auto deadline = state->deadline;
  return deadline && now >= *deadline && state->TryFinish(RequestOutcome::kDeadlineExceeded);
}

}  // namespace a2a::server::engine
