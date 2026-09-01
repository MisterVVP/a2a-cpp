// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace a2a::core::subscription_diagnostics {

enum class Phase : std::size_t {
  kCancelDispatch,
  kTerminalStoreUpdate,
  kTerminalPublication,
  kSubscriberResume,
  kProtoToJson,
  kFrameConstruction,
  kHttpDelivery,
  kClientTerminalObservation,
  kStreamFinalization,
  kCount,
};

inline constexpr std::size_t kPhaseCount = static_cast<std::size_t>(Phase::kCount);

struct PhaseAggregate final {
  std::uint64_t count = 0;
  std::uint64_t elapsed_nanoseconds = 0;
  std::uint64_t maximum_nanoseconds = 0;
};

using Snapshot = std::array<PhaseAggregate, kPhaseCount>;

[[nodiscard]] bool IsEnabled() noexcept;
void Record(Phase phase, std::chrono::steady_clock::duration elapsed) noexcept;
[[nodiscard]] Snapshot TakeSnapshot() noexcept;

class ScopedTimer final {
 public:
  explicit ScopedTimer(Phase phase, bool relevant = true) noexcept;
  ~ScopedTimer();

 private:
  Phase phase_;
  bool enabled_;
  std::chrono::steady_clock::time_point started_;
};

}  // namespace a2a::core::subscription_diagnostics
