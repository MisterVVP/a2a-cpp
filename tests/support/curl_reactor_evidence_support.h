// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <charconv>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "curl_reactor_test_support.h"

namespace a2a::http::testing {

inline constexpr std::string_view kEvidenceDiagnosticsOption = "--diagnostics";
inline constexpr std::string_view kEvidenceReuseOption = "--reuse-client";
inline constexpr std::string_view kEvidenceWarmupOption = "--warmup";
inline constexpr std::string_view kEvidenceCompleteOnlyOption = "--complete-only";
inline constexpr std::string_view kEvidenceRepetitionsOption = "--repetitions";
inline constexpr std::size_t kEvidenceDefaultRepetitions = 20U;
inline constexpr std::size_t kEvidenceMaximumRepetitions = 1000U;
inline constexpr int kEvidenceDiagnosticPrecision = 6;
inline constexpr double kEvidenceMillisecondsPerSecond = 1000.0;
inline constexpr double kEvidenceFileTimeTicksPerMillisecond = 10000.0;
inline constexpr std::string_view kEvidenceIdQuery = "?evidence_id=";
inline constexpr std::string_view kEvidenceFreshClient = "fresh";
inline constexpr std::string_view kEvidenceReusedClient = "reused";
inline constexpr std::string_view kEvidenceCompleteMode = "complete";
inline constexpr std::string_view kEvidenceCancelMode = "cancel";
inline constexpr std::string_view kEvidenceWindowsBackend = "wsapoll";
inline constexpr std::string_view kEvidencePortableBackend = "portable";
inline constexpr std::string_view kEvidenceLinuxBackend = "epoll";
inline constexpr std::string_view kEvidenceDiagnosticsFailure = "invalid reactor diagnostic sample";
inline constexpr std::string_view kEvidenceDiagnosticsFileFailure = "cannot write reactor diagnostics";
inline constexpr std::string_view kEvidenceCpuTimeFailure = "cannot read process CPU time";
inline constexpr std::string_view kEvidenceUsage =
    "usage: curl_reactor_evidence [--diagnostics FILE] [--reuse-client] [--warmup] [--complete-only] [--repetitions N]";

struct CurlEvidenceOptions final {
  std::string diagnostics_path;
  bool reuse_client = false;
  bool warmup = false;
  bool complete_only = false;
  std::size_t repetitions = kEvidenceDefaultRepetitions;

  [[nodiscard]] std::string_view client_lifetime() const noexcept {
    return reuse_client ? kEvidenceReusedClient : kEvidenceFreshClient;
  }
};

inline CurlEvidenceOptions ParseCurlEvidenceOptions(int argc, char** argv) {
  CurlEvidenceOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == kEvidenceDiagnosticsOption && index + 1 < argc) {
      options.diagnostics_path = argv[++index];
    } else if (argument == kEvidenceRepetitionsOption && index + 1 < argc) {
      const std::string_view value(argv[++index]);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.repetitions);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || options.repetitions == 0U ||
          options.repetitions > kEvidenceMaximumRepetitions) {
        throw std::runtime_error(std::string(kEvidenceUsage));
      }
    } else if (argument == kEvidenceReuseOption) {
      options.reuse_client = true;
    } else if (argument == kEvidenceWarmupOption) {
      options.warmup = true;
    } else if (argument == kEvidenceCompleteOnlyOption) {
      options.complete_only = true;
    } else {
      throw std::runtime_error(std::string(kEvidenceUsage));
    }
  }
  return options;
}

inline double EvidenceMilliseconds(CurlStreamProbe::Clock::duration elapsed) {
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

inline double EvidenceProcessCpuMilliseconds() {
#ifdef _WIN32
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == 0) {
    throw std::runtime_error(std::string(kEvidenceCpuTimeFailure));
  }
  ULARGE_INTEGER kernel_ticks{};
  kernel_ticks.LowPart = kernel.dwLowDateTime;
  kernel_ticks.HighPart = kernel.dwHighDateTime;
  ULARGE_INTEGER user_ticks{};
  user_ticks.LowPart = user.dwLowDateTime;
  user_ticks.HighPart = user.dwHighDateTime;
  return static_cast<double>(kernel_ticks.QuadPart + user_ticks.QuadPart) / kEvidenceFileTimeTicksPerMillisecond;
#else
  const auto value = std::clock();
  if (value == static_cast<std::clock_t>(-1)) {
    throw std::runtime_error(std::string(kEvidenceCpuTimeFailure));
  }
  return static_cast<double>(value) * kEvidenceMillisecondsPerSecond / static_cast<double>(CLOCKS_PER_SEC);
#endif
}

struct CurlEvidenceClientTiming final {
  CurlStreamProbe::Clock::time_point started;
  CurlStreamProbe::Clock::time_point api_return;
  CurlStreamProbe::Clock::time_point metadata;
  CurlStreamProbe::Clock::time_point first;
  CurlStreamProbe::Clock::time_point finished;
};

struct CurlEvidenceBatch final {
  std::size_t streams;
  std::size_t repetition;
  bool cancel;
  bool warmup;
  bool valid = true;
  double cpu_before = 0.0;
  double cpu_after_first = 0.0;
  CurlStreamProbe::Clock::time_point origin;
  std::pair<std::size_t, std::size_t> reactors_before;
  std::pair<std::size_t, std::size_t> reactors_after_submit;
  std::pair<std::size_t, std::size_t> reactors_after_first;
  std::pair<std::size_t, std::size_t> reactors_after_teardown;
  std::vector<std::optional<CurlServerTiming>> server;
  std::vector<CurlEvidenceClientTiming> client;

  CurlEvidenceBatch(std::size_t count, std::size_t repeat, bool cancellation, bool warming)
      : streams(count), repetition(repeat), cancel(cancellation), warmup(warming), server(count) {
    client.reserve(count);
  }

  // The server invokes this before publishing the connection under its mutex.
  // WaitForStreams therefore establishes visibility before the batch is read.
  void RecordServer(const CurlServerTiming& timing, std::string_view request) noexcept {
    const auto marker = request.find(kEvidenceIdQuery);
    if (marker == std::string_view::npos) {
      valid = false;
      return;
    }
    std::size_t index = 0U;
    const auto parsed =
        std::from_chars(request.data() + marker + kEvidenceIdQuery.size(), request.data() + request.size(), index);
    if (parsed.ec != std::errc{} || index >= server.size() || server[index].has_value()) {
      valid = false;
      return;
    }
    server[index] = timing;
  }

  void RecordClients(const std::vector<std::shared_ptr<CurlStreamProbe>>& probes) {
    for (const auto& probe : probes) {
      std::lock_guard lock(probe->mutex);
      client.push_back({.started = probe->started,
                        .api_return = probe->api_return,
                        .metadata = probe->metadata_observed,
                        .first = probe->first_event,
                        .finished = probe->finished});
    }
  }
};

inline void WriteEvidenceClient(std::ostream& output, const CurlEvidenceBatch& batch, std::size_t index) {
  const auto& server_timing = batch.server.at(index);
  if (!batch.valid || !server_timing.has_value()) {
    throw std::runtime_error(std::string(kEvidenceDiagnosticsFailure));
  }
  const auto& server = server_timing.value();
  const auto& client = batch.client[index];
  const auto elapsed = [&batch](auto time) { return EvidenceMilliseconds(time - batch.origin); };
  output << R"({"id":)" << index << R"(,"submit_ms":)" << elapsed(client.started) << R"(,"api_return_ms":)"
         << elapsed(client.api_return) << R"(,"accept_begin_ms":)" << elapsed(server.accept_begin)
         << R"(,"accepted_ms":)" << elapsed(server.accepted) << R"(,"request_ready_ms":)"
         << elapsed(server.request_ready) << R"(,"send_begin_ms":)" << elapsed(server.send_begin)
         << R"(,"send_end_ms":)" << elapsed(server.send_end) << R"(,"metadata_ms":)" << elapsed(client.metadata)
         << R"(,"first_ms":)" << elapsed(client.first) << R"(,"finished_ms":)" << elapsed(client.finished) << '}';
}

inline void WriteEvidenceLifecycle(std::ostream& output, std::pair<std::size_t, std::size_t> counters) {
  output << R"({"created":)" << counters.first << R"(,"destroyed":)" << counters.second << '}';
}

inline void WriteEvidenceBatch(std::ostream& output, const CurlEvidenceBatch& batch) {
  output << R"({"streams":)" << batch.streams << R"(,"mode":)"
         << std::quoted(batch.cancel ? kEvidenceCancelMode : kEvidenceCompleteMode) << R"(,"repetition":)"
         << batch.repetition << R"(,"warmup":)" << (batch.warmup ? "true" : "false") << R"(,"reactors_before":)";
  WriteEvidenceLifecycle(output, batch.reactors_before);
  output << R"(,"reactors_after_submit":)";
  WriteEvidenceLifecycle(output, batch.reactors_after_submit);
  output << R"(,"reactors_after_first":)";
  WriteEvidenceLifecycle(output, batch.reactors_after_first);
  output << R"(,"reactors_after_teardown":)";
  WriteEvidenceLifecycle(output, batch.reactors_after_teardown);
  output << R"(,"process_cpu_ms":)" << batch.cpu_after_first - batch.cpu_before;
  output << R"(,"clients":[)";
  for (std::size_t index = 0; index < batch.client.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    WriteEvidenceClient(output, batch, index);
  }
  output << "]}";
}

inline std::string EvidenceCompiler() {
#ifdef __clang__
  return __clang_version__;
#elif defined(_MSC_FULL_VER)
  std::string compiler("MSVC ");
  compiler.append(std::to_string(_MSC_FULL_VER));
  return compiler;
#elif defined(__VERSION__)
  return __VERSION__;
#else
  constexpr std::string_view kUnknownCompiler = "unknown";
  return std::string(kUnknownCompiler);
#endif
}

inline std::string_view EvidenceBackend() {
#if defined(_WIN32) && !defined(A2A_TEST_CURL_PORTABLE_REACTOR)
  return kEvidenceWindowsBackend;
#elif defined(__linux__)
  return kEvidenceLinuxBackend;
#else
  return kEvidencePortableBackend;
#endif
}

inline void WriteCurlDiagnostics(const CurlEvidenceOptions& options, unsigned int timer_period,
                                 const std::vector<CurlEvidenceBatch>& batches) {
  if (options.diagnostics_path.empty()) {
    return;
  }
  std::ofstream output(options.diagnostics_path);
  if (!output) {
    throw std::runtime_error(std::string(kEvidenceDiagnosticsFileFailure));
  }
  output << std::fixed << std::setprecision(kEvidenceDiagnosticPrecision) << R"({"compiler":)"
         << std::quoted(EvidenceCompiler()) << R"(,"backend":)" << std::quoted(EvidenceBackend()) << R"(,"libcurl":)"
         << std::quoted(curl_version()) << R"(,"hardware_threads":)" << std::thread::hardware_concurrency()
         << R"(,"timer_period_ms":)" << timer_period << R"(,"client_lifetime":)"
         << std::quoted(options.client_lifetime()) << R"(,"batches":[)";
  for (std::size_t index = 0; index < batches.size(); ++index) {
    if (index != 0U) {
      output << ',';
    }
    WriteEvidenceBatch(output, batches[index]);
  }
  output << "]}\n";
  output.close();
  if (!output) {
    throw std::runtime_error(std::string(kEvidenceDiagnosticsFileFailure));
  }
}

}  // namespace a2a::http::testing
