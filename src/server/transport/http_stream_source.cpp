// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/http_stream_source.h"

#include <chrono>
#include <utility>

#include "a2a/core/task_states.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif

namespace a2a::server {

HttpStreamSource::HttpStreamSource(std::shared_ptr<ServerStreamSession> session, EventEncoder encode_event,
                                   ErrorEncoder encode_error)
    : session_(std::move(session)), encode_event_(std::move(encode_event)), encode_error_(std::move(encode_error)) {}
HttpStreamSource::~HttpStreamSource() { Cancel(); }

core::Result<std::optional<std::string>> HttpStreamSource::Next() {
  if (finished_) {
    return std::optional<std::string>{};
  }
  auto event = session_->IsLive() ? session_->NextFor(std::chrono::milliseconds::zero()) : session_->Next();
  if (!event.ok()) {
    finished_ = true;
    if (!encode_error_) {
      return event.error();
    }
    auto encoded = encode_error_(event.error());
    if (!encoded.ok()) {
      return encoded.error();
    }
    return std::optional<std::string>(std::move(encoded.value()));
  }
  const auto& response = event.value();
  if (!response.has_value()) {
    finished_ = !session_->IsLive();
    return std::optional<std::string>{};
  }
  auto encoded = encode_event_(response.value());
  if (!encoded.ok()) {
    return encoded.error();
  }
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  if (core::subscription_diagnostics::IsEnabled() && response->has_status_update() &&
      core::IsTerminalTaskState(response->status_update().status().state())) {
    terminal_delivery_started_ = std::chrono::steady_clock::now();
  }
#endif
  return std::optional<std::string>(std::move(encoded.value()));
}

void HttpStreamSource::CompleteDelivery() noexcept {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  if (terminal_delivery_started_) {
    core::subscription_diagnostics::Record(core::subscription_diagnostics::Phase::kHttpDelivery,
                                           std::chrono::steady_clock::now() - *terminal_delivery_started_);
    terminal_delivery_started_.reset();
  }
#endif
}

bool HttpStreamSource::IsLive() const noexcept { return !finished_ && session_->IsLive(); }
bool HttpStreamSource::SetReadyCallback(std::function<void()> callback) {
  return session_->SetReadyCallback(std::move(callback));
}
void HttpStreamSource::Cancel() noexcept {
  (void)session_->SetReadyCallback({});
  session_->Cancel();
  finished_ = true;
}

}  // namespace a2a::server
