// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <exception>
#include <iostream>
#include <string_view>

#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif
#include "sut/sut_runtime.h"

namespace {

constexpr std::string_view kPerformanceSutName = "Performance SUT";
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
constexpr std::string_view kSubscriptionDiagnosticsPrefix = "A2A_SUBSCRIPTION_SERVER_DIAGNOSTICS";

void EmitSubscriptionDiagnostics() {
  if (!a2a::core::subscription_diagnostics::IsEnabled()) {
    return;
  }
  const auto snapshot = a2a::core::subscription_diagnostics::TakeSnapshot();
  std::cout << kSubscriptionDiagnosticsPrefix;
  for (std::size_t index = 0; index < snapshot.size(); ++index) {
    const auto& aggregate = snapshot[index];
    const auto phase_name = a2a::core::subscription_diagnostics::kPhaseNames[index];
    std::cout << ' ' << phase_name << "_count=" << aggregate.count << ' ' << phase_name
              << "_total_ns=" << aggregate.elapsed_nanoseconds << ' ' << phase_name
              << "_max_ns=" << aggregate.maximum_nanoseconds;
  }
  std::cout << '\n' << std::flush;
}
#endif

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    a2a::tests::sut::SutRuntimeOptions options{.display_name = kPerformanceSutName, .enable_http_diagnostics = true};
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    options.emit_shutdown_diagnostics = EmitSubscriptionDiagnostics;
#endif
    return a2a::tests::sut::RunSutRuntime(argc, argv, options);
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled performance SUT exception: " << ex.what() << '\n';
    return 1;
  }
}
