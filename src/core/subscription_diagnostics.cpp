// SPDX-License-Identifier: Apache-2.0

#include "a2a/core/subscription_diagnostics.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <string_view>

namespace a2a::core::subscription_diagnostics {
namespace {

constexpr std::string_view kEnabledValue = "1";
constexpr char kDiagnosticsEnvironmentVariable[] = "A2A_SUBSCRIPTION_DIAGNOSTICS";

struct AtomicAggregate final {
  std::atomic_uint64_t count = 0;
  std::atomic_uint64_t elapsed_nanoseconds = 0;
  std::atomic_uint64_t maximum_nanoseconds = 0;
};

std::array<AtomicAggregate, kPhaseCount> g_aggregates;

}  // namespace

bool IsEnabled() noexcept {
  static const bool enabled = [] {
    const char* value = std::getenv(kDiagnosticsEnvironmentVariable);
    return value != nullptr && value == kEnabledValue;
  }();
  return enabled;
}

void Record(Phase phase, std::chrono::steady_clock::duration elapsed) noexcept {
  const auto nanoseconds = static_cast<std::uint64_t>(
      std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  auto& aggregate = g_aggregates[static_cast<std::size_t>(phase)];
  aggregate.count.fetch_add(1, std::memory_order_relaxed);
  aggregate.elapsed_nanoseconds.fetch_add(nanoseconds, std::memory_order_relaxed);
  auto maximum = aggregate.maximum_nanoseconds.load(std::memory_order_relaxed);
  while (maximum < nanoseconds && !aggregate.maximum_nanoseconds.compare_exchange_weak(
                                      maximum, nanoseconds, std::memory_order_relaxed)) {
  }
}

Snapshot TakeSnapshot() noexcept {
  Snapshot snapshot;
  for (std::size_t index = 0; index < snapshot.size(); ++index) {
    auto& aggregate = g_aggregates[index];
    snapshot[index] = {.count = aggregate.count.exchange(0, std::memory_order_relaxed),
                       .elapsed_nanoseconds = aggregate.elapsed_nanoseconds.exchange(0, std::memory_order_relaxed),
                       .maximum_nanoseconds = aggregate.maximum_nanoseconds.exchange(0, std::memory_order_relaxed)};
  }
  return snapshot;
}

ScopedTimer::ScopedTimer(Phase phase, bool relevant) noexcept
    : phase_(phase),
      enabled_(relevant && IsEnabled()),
      started_(enabled_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}) {}

ScopedTimer::~ScopedTimer() {
  if (enabled_) {
    Record(phase_, std::chrono::steady_clock::now() - started_);
  }
}

}  // namespace a2a::core::subscription_diagnostics
