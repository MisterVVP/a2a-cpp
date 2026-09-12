// SPDX-License-Identifier: Apache-2.0

#include "core/subscription_diagnostics.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace {

constexpr std::chrono::nanoseconds kFirstDiagnosticDuration{11};
constexpr std::chrono::nanoseconds kSecondDiagnosticDuration{29};
constexpr std::array<std::string_view, a2a::core::subscription_diagnostics::kPhaseCount> kExpectedPhaseNames = {
    "server_cancel_task_total",
    "terminal_store_update",
    "terminal_publication_total",
    "subscriber_resume_callback",
    "proto_to_json",
    "frame_construction",
    "http_delivery",
    "client_terminal_observer_callback",
    "client_completion_callback"};

TEST(SubscriptionDiagnosticsTest, PhaseNamesRemainStableForPerformanceReports) {
  EXPECT_EQ(a2a::core::subscription_diagnostics::kPhaseNames, kExpectedPhaseNames);
}

TEST(SubscriptionDiagnosticsTest, AggregatesAndResetsWithoutSharedLocks) {
  using a2a::core::subscription_diagnostics::Phase;
  (void)a2a::core::subscription_diagnostics::TakeSnapshot();
  a2a::core::subscription_diagnostics::Record(Phase::kTerminalPublicationTotal, kFirstDiagnosticDuration);
  a2a::core::subscription_diagnostics::Record(Phase::kTerminalPublicationTotal, kSecondDiagnosticDuration);

  const auto snapshot = a2a::core::subscription_diagnostics::TakeSnapshot();
  const auto& publication = snapshot[static_cast<std::size_t>(Phase::kTerminalPublicationTotal)];
  EXPECT_EQ(publication.count, 2U);
  EXPECT_EQ(publication.elapsed_nanoseconds,
            static_cast<std::uint64_t>((kFirstDiagnosticDuration + kSecondDiagnosticDuration).count()));
  EXPECT_EQ(publication.maximum_nanoseconds, static_cast<std::uint64_t>(kSecondDiagnosticDuration.count()));
  EXPECT_EQ(
      a2a::core::subscription_diagnostics::TakeSnapshot()[static_cast<std::size_t>(Phase::kTerminalPublicationTotal)]
          .count,
      0U);
}

}  // namespace
