// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "a2a/core/result.h"
#include "a2a/server/server_stream_session.h"

namespace a2a::server {

// Resumable SSE production. Calls are serialized by the caller, including
// cancellation. Ready callbacks only schedule work; they must never call Next.
// A missing event with IsLive() true means wait for readiness or a heartbeat.
class HttpStreamSource final {
 public:
  using EventEncoder = std::function<core::Result<std::string>(const lf::a2a::v1::StreamResponse&)>;
  using ErrorEncoder = std::function<core::Result<std::string>(const core::Error&)>;

  HttpStreamSource(std::shared_ptr<ServerStreamSession> session, EventEncoder encode_event,
                   ErrorEncoder encode_error = {});
  ~HttpStreamSource();
  HttpStreamSource(const HttpStreamSource&) = delete;
  HttpStreamSource& operator=(const HttpStreamSource&) = delete;
  HttpStreamSource(HttpStreamSource&&) = delete;
  HttpStreamSource& operator=(HttpStreamSource&&) = delete;

  [[nodiscard]] core::Result<std::optional<std::string>> Next();
  [[nodiscard]] bool IsLive() const noexcept;
  [[nodiscard]] bool SetReadyCallback(std::function<void()> callback);
  void SetPendingEventByteLimit(std::size_t limit) { session_->SetPendingEventByteLimit(limit); }
  // Called after the queued bytes containing a terminal event reach the socket.
  // This is serialized with Next()/Cancel() by the transport.
  void CompleteDelivery() noexcept;
  void Cancel() noexcept;

 private:
  std::shared_ptr<ServerStreamSession> session_;
  EventEncoder encode_event_;
  ErrorEncoder encode_error_;
  bool finished_ = false;
  std::optional<std::chrono::steady_clock::time_point> terminal_delivery_started_;
};

}  // namespace a2a::server
