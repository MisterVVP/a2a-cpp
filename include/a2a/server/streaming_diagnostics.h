// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "a2a/v1/a2a.pb.h"

namespace a2a::server::streaming_diagnostics {

enum class Phase : std::size_t {
  kCancelDispatchToPublish,
  kPublishToNotify,
  kNotifyToObserve,
  kObserveToSerialize,
  kSerialize,
  kFrame,
  kSocketWrite,
  kWriteToFinalize,
  kCount,
};

inline constexpr std::size_t kPhaseCount = static_cast<std::size_t>(Phase::kCount);

struct Snapshot final {
  std::array<std::uint64_t, kPhaseCount> total_nanoseconds{};
  std::array<std::uint64_t, kPhaseCount> maximum_nanoseconds{};
  std::array<std::uint64_t, kPhaseCount> sample_count{};
};

void SetEnabled(bool enabled) noexcept;
[[nodiscard]] bool IsEnabled() noexcept;
void Reset() noexcept;
[[nodiscard]] Snapshot Take() noexcept;

void CancelDispatchBegins() noexcept;
[[nodiscard]] std::int64_t TerminalPublicationBegins() noexcept;
[[nodiscard]] std::int64_t SubscriberNotified(std::int64_t publication_started) noexcept;
void TerminalEventObserved(std::int64_t notification_time) noexcept;
[[nodiscard]] std::int64_t SerializationBegins(const lf::a2a::v1::StreamResponse& event) noexcept;
void SerializationCompletes(std::int64_t serialization_started) noexcept;
void FramingCompletes(bool terminal_event) noexcept;
[[nodiscard]] std::int64_t SocketWriteBegins(bool terminal_event) noexcept;
void SocketWriteCompletes(std::int64_t write_started) noexcept;
void StreamFinalized() noexcept;

[[nodiscard]] bool IsTerminalEvent(const lf::a2a::v1::StreamResponse& event) noexcept;

}  // namespace a2a::server::streaming_diagnostics
