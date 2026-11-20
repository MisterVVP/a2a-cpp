// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

#include "sut/performance_sut_diagnostics.h"
#include "sut/sut_runtime.h"

namespace {

constexpr std::string_view kPerformanceSutName = "Performance SUT";
constexpr auto kServerModeEnvironment = "A2A_PERF_SERVER_MODE";
constexpr auto kDelayEnvironment = "A2A_PERF_GET_TASK_DELAY_MS";
constexpr auto kHttpPendingEnvironment = "A2A_PERF_HTTP_PENDING_LIMIT";
constexpr auto kDispatcherThreadsEnvironment = "A2A_PERF_DISPATCHER_THREADS";
constexpr auto kIngressEnvironment = "A2A_PERF_INGRESS";
constexpr auto kSeedPipelineEnvironment = "A2A_PERF_SEED_PIPELINE_TASK";
constexpr int kSeedPipelineEnabled = 1;
constexpr int kMaxDelayMs = 1000;
constexpr int kMaxHttpPending = 1024;
constexpr int kMaxDispatcherThreads = 64;
constexpr auto kStoreBackendEnvironment = "A2A_TCK_STORE_BACKEND";
constexpr std::string_view kSyncMode = "sync";
constexpr std::string_view kListenerMode = "listener";
constexpr std::string_view kImmediateMode = "immediate";
constexpr std::string_view kAsyncMode = "async";
constexpr std::string_view kPipelineSyncMode = "pipeline-sync";
constexpr std::string_view kPipelineAsyncMode = "pipeline-async";
constexpr std::string_view kSharedIngress = "shared";
constexpr std::string_view kSeparateIngress = "separate";
constexpr std::string_view kInMemoryBackend = "inmemory";
constexpr std::string_view kSyncExecutor = "synchronous";
constexpr std::string_view kNativeExecutor = "native_coroutine";
constexpr std::string_view kBlockingExecutor = "blocking_adapter";

a2a::tests::sut::SutReadFixture ReadFixture(std::string_view mode) {
  using a2a::tests::sut::SutReadFixture;
  if (mode == kListenerMode) {
    return SutReadFixture::kListener;
  }
  if (mode == kImmediateMode) {
    return SutReadFixture::kDispatcher;
  }
  if (mode == kPipelineSyncMode || mode == kPipelineAsyncMode) {
    return SutReadFixture::kPipeline;
  }
  return SutReadFixture::kDisabled;
}

int ReadLimit(const char* name, int maximum) {
  const char* configured = std::getenv(name);
  if (configured == nullptr) {
    return 0;
  }
  const auto* end = configured + std::string_view(configured).size();
  int parsed = 0;
  const auto result = std::from_chars(configured, end, parsed);
  if (result.ec != std::errc{} || result.ptr != end || parsed < 0 || parsed > maximum) {
    throw std::invalid_argument(name);
  }
  return parsed;
}

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    const auto* configured_mode = std::getenv(kServerModeEnvironment);
    const std::string_view mode = configured_mode == nullptr ? kSyncMode : configured_mode;
    const auto fixture = ReadFixture(mode);
    const bool isolated_read = fixture != a2a::tests::sut::SutReadFixture::kDisabled;
    const bool asynchronous = mode == kAsyncMode || mode == kPipelineAsyncMode || mode == kImmediateMode;
    if (!isolated_read && mode != kSyncMode && !asynchronous) {
      std::cerr << "Invalid performance server execution mode\n";
      return 1;
    }
#ifndef A2A_PERFORMANCE_ASIO
    if (isolated_read || mode == kAsyncMode) {
      std::cerr << "Async performance SUT requires A2A_ENABLE_ASIO and diagnostics disabled\n";
      return 1;
    }
#endif
    const auto* backend = std::getenv(kStoreBackendEnvironment);
    const bool in_memory = backend == nullptr || std::string_view(backend) == kInMemoryBackend;
    auto executor = kSyncExecutor;
    if (asynchronous) {
      executor = in_memory ? kNativeExecutor : kBlockingExecutor;
    }
    const auto* configured_ingress = std::getenv(kIngressEnvironment);
    const std::string_view ingress = configured_ingress == nullptr ? kSharedIngress : configured_ingress;
    if (ingress != kSharedIngress && ingress != kSeparateIngress) {
      throw std::invalid_argument(kIngressEnvironment);
    }
    const auto dispatcher_threads =
        static_cast<std::size_t>(ReadLimit(kDispatcherThreadsEnvironment, kMaxDispatcherThreads));
    std::cerr << "A2A_PERFORMANCE_MODE server=" << mode << " executor=" << executor << " ingress=" << ingress
              << " dispatcher_threads=" << dispatcher_threads << '\n';
    a2a::tests::sut::PerformanceSutDiagnostics diagnostics;
    return a2a::tests::sut::RunSutRuntime(
        argc, argv,
        {.display_name = kPerformanceSutName,
         .observer = &diagnostics,
         .asynchronous = asynchronous,
         .allow_opaque_input = true,
         .get_task_delay = std::chrono::milliseconds(ReadLimit(kDelayEnvironment, kMaxDelayMs)),
         .http_pending_limit = static_cast<std::size_t>(ReadLimit(kHttpPendingEnvironment, kMaxHttpPending)),
         .read_fixture = fixture,
         .dispatcher_threads = dispatcher_threads,
         .ingress =
             ingress == kSharedIngress ? a2a::tests::sut::SutIngress::kShared : a2a::tests::sut::SutIngress::kSeparate,
         .seed_pipeline_task = ReadLimit(kSeedPipelineEnvironment, kSeedPipelineEnabled) == kSeedPipelineEnabled});
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled performance SUT exception: " << ex.what() << '\n';
    return 1;
  }
}
