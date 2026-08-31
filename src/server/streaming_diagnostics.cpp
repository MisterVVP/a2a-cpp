// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/streaming_diagnostics.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "a2a/core/task_states.h"

namespace a2a::server::streaming_diagnostics {
namespace {

struct AtomicAggregate final {
  std::atomic_uint64_t total_nanoseconds{0U};
  std::atomic_uint64_t maximum_nanoseconds{0U};
  std::atomic_uint64_t sample_count{0U};
};

std::atomic_bool g_enabled{false};
std::array<AtomicAggregate, kPhaseCount> g_aggregates;
thread_local std::int64_t g_cancel_dispatch_started = 0;
thread_local std::int64_t g_terminal_stage_started = 0;

std::int64_t NowNanoseconds() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void Record(Phase phase, std::int64_t started, std::int64_t completed) noexcept {
  if (started == 0 || completed < started) {
    return;
  }
  const auto elapsed = static_cast<std::uint64_t>(completed - started);
  auto& aggregate = g_aggregates[static_cast<std::size_t>(phase)];
  aggregate.total_nanoseconds.fetch_add(elapsed, std::memory_order_relaxed);
  aggregate.sample_count.fetch_add(1U, std::memory_order_relaxed);
  auto maximum = aggregate.maximum_nanoseconds.load(std::memory_order_relaxed);
  while (maximum < elapsed && !aggregate.maximum_nanoseconds.compare_exchange_weak(
                                  maximum, elapsed, std::memory_order_relaxed, std::memory_order_relaxed)) {
  }
}

void CompleteCurrentStage(Phase phase) noexcept {
  if (g_terminal_stage_started == 0) {
    return;
  }
  const auto completed = NowNanoseconds();
  Record(phase, g_terminal_stage_started, completed);
  g_terminal_stage_started = completed;
}

}  // namespace

void SetEnabled(bool enabled) noexcept { g_enabled.store(enabled, std::memory_order_relaxed); }

bool IsEnabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void Reset() noexcept {
  for (auto& aggregate : g_aggregates) {
    aggregate.total_nanoseconds.store(0U, std::memory_order_relaxed);
    aggregate.maximum_nanoseconds.store(0U, std::memory_order_relaxed);
    aggregate.sample_count.store(0U, std::memory_order_relaxed);
  }
}

Snapshot Take() noexcept {
  Snapshot snapshot;
  for (std::size_t index = 0; index < kPhaseCount; ++index) {
    snapshot.total_nanoseconds[index] = g_aggregates[index].total_nanoseconds.load(std::memory_order_relaxed);
    snapshot.maximum_nanoseconds[index] = g_aggregates[index].maximum_nanoseconds.load(std::memory_order_relaxed);
    snapshot.sample_count[index] = g_aggregates[index].sample_count.load(std::memory_order_relaxed);
  }
  return snapshot;
}

void CancelDispatchBegins() noexcept {
  if (IsEnabled()) {
    g_cancel_dispatch_started = NowNanoseconds();
  }
}

std::int64_t TerminalPublicationBegins() noexcept {
  if (!IsEnabled()) {
    return 0;
  }
  const auto started = NowNanoseconds();
  Record(Phase::kCancelDispatchToPublish, g_cancel_dispatch_started, started);
  g_cancel_dispatch_started = 0;
  return started;
}

std::int64_t SubscriberNotified(std::int64_t publication_started) noexcept {
  if (publication_started == 0) {
    return 0;
  }
  const auto notified = NowNanoseconds();
  Record(Phase::kPublishToNotify, publication_started, notified);
  return notified;
}

void TerminalEventObserved(std::int64_t notification_time) noexcept {
  if (notification_time == 0) {
    return;
  }
  const auto observed = NowNanoseconds();
  Record(Phase::kNotifyToObserve, notification_time, observed);
  g_terminal_stage_started = observed;
}

std::int64_t SerializationBegins(const lf::a2a::v1::StreamResponse& event) noexcept {
  if (!IsEnabled() || !IsTerminalEvent(event) || g_terminal_stage_started == 0) {
    return 0;
  }
  const auto started = NowNanoseconds();
  Record(Phase::kObserveToSerialize, g_terminal_stage_started, started);
  g_terminal_stage_started = started;
  return started;
}

void SerializationCompletes(std::int64_t serialization_started) noexcept {
  if (serialization_started == 0) {
    return;
  }
  const auto completed = NowNanoseconds();
  Record(Phase::kSerialize, serialization_started, completed);
  g_terminal_stage_started = completed;
}

void FramingCompletes(bool terminal_event) noexcept {
  if (terminal_event) {
    CompleteCurrentStage(Phase::kFrame);
  }
}

std::int64_t SocketWriteBegins(bool terminal_event) noexcept {
  return terminal_event && IsEnabled() && g_terminal_stage_started != 0 ? NowNanoseconds() : 0;
}

void SocketWriteCompletes(std::int64_t write_started) noexcept {
  if (write_started == 0) {
    return;
  }
  const auto completed = NowNanoseconds();
  Record(Phase::kSocketWrite, write_started, completed);
  g_terminal_stage_started = completed;
}

void StreamFinalized() noexcept {
  CompleteCurrentStage(Phase::kWriteToFinalize);
  g_terminal_stage_started = 0;
}

bool IsTerminalEvent(const lf::a2a::v1::StreamResponse& event) noexcept {
  return event.has_status_update() && core::IsTerminalTaskState(event.status_update().status().state());
}

}  // namespace a2a::server::streaming_diagnostics
