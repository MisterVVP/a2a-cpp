// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

#include "sut/performance_sut_diagnostics.h"
#include "sut/sut_runtime.h"

namespace {

constexpr std::string_view kPerformanceSutName = "Performance SUT";
constexpr auto kServerModeEnvironment = "A2A_PERF_SERVER_MODE";
constexpr auto kStoreBackendEnvironment = "A2A_TCK_STORE_BACKEND";
constexpr std::string_view kSyncMode = "sync";
constexpr std::string_view kAsyncMode = "async";
constexpr std::string_view kInMemoryBackend = "inmemory";
constexpr std::string_view kSyncExecutor = "synchronous";
constexpr std::string_view kNativeExecutor = "native_coroutine";
constexpr std::string_view kBlockingExecutor = "blocking_adapter";

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    const auto* configured_mode = std::getenv(kServerModeEnvironment);
    const std::string_view mode = configured_mode == nullptr ? kSyncMode : configured_mode;
    if (mode != kSyncMode && mode != kAsyncMode) {
      std::cerr << "Invalid performance server execution mode\n";
      return 1;
    }
#ifndef A2A_PERFORMANCE_ASIO
    if (mode == kAsyncMode) {
      std::cerr << "Async performance SUT requires A2A_ENABLE_ASIO and diagnostics disabled\n";
      return 1;
    }
#endif
    const auto* backend = std::getenv(kStoreBackendEnvironment);
    const bool in_memory = backend == nullptr || std::string_view(backend) == kInMemoryBackend;
    auto executor = kSyncExecutor;
    if (mode == kAsyncMode) {
      executor = in_memory ? kNativeExecutor : kBlockingExecutor;
    }
    std::cerr << "A2A_PERFORMANCE_MODE server=" << mode << " executor=" << executor << '\n';
    a2a::tests::sut::PerformanceSutDiagnostics diagnostics;
    return a2a::tests::sut::RunSutRuntime(
        argc, argv,
        {.display_name = kPerformanceSutName, .observer = &diagnostics, .asynchronous = mode == kAsyncMode});
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled performance SUT exception: " << ex.what() << '\n';
    return 1;
  }
}
