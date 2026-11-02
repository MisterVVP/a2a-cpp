// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "support/curl_reactor_test_support.h"

#ifdef _WIN32
#include <mmsystem.h>
#endif

namespace {

using namespace a2a::http::testing;
constexpr std::array<std::size_t, 4> kStreamCounts{1U, 16U, 64U, 96U};
constexpr std::size_t kRepetitions = 20U;
constexpr double kP95 = 0.95;
constexpr double kP99 = 0.99;
constexpr std::size_t kMaximumThreadGrowth = 8U;
constexpr std::string_view kEvidenceFailure = "reactor evidence workload failed";

#ifdef _WIN32
constexpr UINT kEvidenceTimerPeriodMs = 1U;
constexpr std::string_view kTimerPeriodFailure = "cannot set Windows evidence timer period";

// Control the Windows timer quantum in both executables. Default ~16 ms ticks
// can dominate first-event timing even when cancellation itself is sub-ms.
class EvidenceTimerPeriod final {
 public:
  EvidenceTimerPeriod() {
    if (timeBeginPeriod(kEvidenceTimerPeriodMs) != TIMERR_NOERROR) {
      throw std::runtime_error(std::string(kTimerPeriodFailure));
    }
  }
  ~EvidenceTimerPeriod() { (void)timeEndPeriod(kEvidenceTimerPeriodMs); }
  EvidenceTimerPeriod(const EvidenceTimerPeriod&) = delete;
  EvidenceTimerPeriod& operator=(const EvidenceTimerPeriod&) = delete;
};
#else
constexpr unsigned int kEvidenceTimerPeriodMs = 0U;
#endif

struct Samples final {
  std::vector<double> first_ms;
  std::vector<double> finish_ms;
  std::size_t maximum_thread_growth = 0;
};

double Milliseconds(CurlStreamProbe::Clock::duration elapsed) {
  return std::chrono::duration<double, std::milli>(elapsed).count();
}

void CollectFirstEvents(const std::vector<std::shared_ptr<CurlStreamProbe>>& streams, Samples& samples) {
  for (const auto& stream : streams) {
    if (!stream->WaitForChunk()) {
      throw std::runtime_error(std::string(kEvidenceFailure));
    }
    std::lock_guard lock(stream->mutex);
    samples.first_ms.push_back(Milliseconds(stream->first_event - stream->started));
  }
}

void CompleteWorkload(CurlLoopbackServer& server, const std::vector<std::shared_ptr<CurlStreamProbe>>& streams,
                      bool cancel, Samples& samples) {
  if (!cancel) {
    const auto finish_started = CurlStreamProbe::Clock::now();
    for (std::size_t index = 0; index < streams.size(); ++index) {
      if (!server.Finish(index)) {
        throw std::runtime_error(std::string(kEvidenceFailure));
      }
    }
    for (const auto& stream : streams) {
      if (stream->result.wait_for(kReactorTestTimeout) != std::future_status::ready || !stream->result.get().ok()) {
        throw std::runtime_error(std::string(kEvidenceFailure));
      }
      samples.finish_ms.push_back(Milliseconds(stream->finished - finish_started));
    }
    return;
  }
  // Measure one established idle stream's cancellation at a time while peers
  // remain live. Include dispatch completion, not just command submission.
  for (const auto& stream : streams) {
    const auto cancel_started = CurlStreamProbe::Clock::now();
    stream->cancel();
    if (stream->result.wait_for(kReactorTestTimeout) != std::future_status::ready || stream->result.get().ok()) {
      throw std::runtime_error(std::string(kEvidenceFailure));
    }
    samples.finish_ms.push_back(Milliseconds(stream->finished - cancel_started));
  }
}

Samples Measure(std::size_t count, bool cancel) {
  Samples samples;
  for (std::size_t repetition = 0; repetition < kRepetitions; ++repetition) {
    CurlLoopbackServer server(count);
    a2a::http::Client client;
    const auto baseline_threads = ReactorTestThreadCount();
    std::vector<std::shared_ptr<CurlStreamProbe>> streams;
    streams.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
      streams.push_back(StartTestStream(client, server.url()));
    }
    if (!server.WaitForStreams(count)) {
      throw std::runtime_error(std::string(kEvidenceFailure));
    }
    CollectFirstEvents(streams, samples);
    const auto threads = ReactorTestThreadCount();
    const auto growth = threads > baseline_threads ? threads - baseline_threads : 0U;
    samples.maximum_thread_growth = (std::max)(samples.maximum_thread_growth, growth);
    if (growth > kMaximumThreadGrowth) {
      throw std::runtime_error(std::string(kEvidenceFailure));
    }
    CompleteWorkload(server, streams, cancel, samples);
  }
  return samples;
}

double Percentile(std::vector<double> values, double quantile) {
  std::ranges::sort(values);
  const auto index = static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(values.size()))) - 1U;
  return values.at(index);
}

void PrintSamples(std::size_t count, bool cancel, const Samples& samples) {
  std::cout << R"({"streams":)" << count << R"(,"mode":")" << (cancel ? "cancel" : "complete") << R"(","samples":)"
            << samples.finish_ms.size() << R"(,"first_p95_ms":)" << Percentile(samples.first_ms, kP95)
            << R"(,"finish_p95_ms":)" << Percentile(samples.finish_ms, kP95) << R"(,"finish_p99_ms":)"
            << Percentile(samples.finish_ms, kP99) << R"(,"maximum_thread_growth":)" << samples.maximum_thread_growth
            << R"(,"timer_period_ms":)" << kEvidenceTimerPeriodMs << R"(,"errors":0})";
}

}  // namespace

int main() {
  try {
#ifdef _WIN32
    const EvidenceTimerPeriod timer_period;
#endif
    std::cout << std::fixed << std::setprecision(3) << "[\n";
    bool first = true;
    for (const auto count : kStreamCounts) {
      for (const bool cancel : {false, true}) {
        if (!first) {
          std::cout << ",\n";
        }
        first = false;
        PrintSamples(count, cancel, Measure(count, cancel));
      }
    }
    std::cout << "\n]\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
