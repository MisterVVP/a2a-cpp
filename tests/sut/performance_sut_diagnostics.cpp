// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/performance_sut_diagnostics.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

#include "a2a/core/http_constants.h"
#include "a2a/server/http_adapter.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif
#include "sut/performance_sut_constants.h"

namespace a2a::tests::sut {
namespace {

constexpr std::string_view kHttpDiagnosticsPrefix = "A2A_HTTP_DIAGNOSTICS";
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
constexpr std::string_view kSubscriptionDiagnosticsPrefix = "A2A_SUBSCRIPTION_SERVER_DIAGNOSTICS";
#endif

}  // namespace

class PerformanceSutDiagnostics::Impl final {
 public:
  class ConnectionObserver final : public SutHttpConnectionObserver {
   public:
    explicit ConnectionObserver(Impl& diagnostics) : diagnostics_(diagnostics) {}
    ~ConnectionObserver() override { ReleaseMeasurement(); }

    [[nodiscard]] bool BeginRequest(const server::HttpServerRequest& request,
                                    server::HttpServerResponse& response) override;
    void FinishRequest(const server::HttpServerResponse& response, bool close_connection,
                       bool handled_by_observer) override;

   private:
    void SynchronizeGeneration();
    void ReleaseMeasurement();

    Impl& diagnostics_;
    bool completed_unary_ = false;
    bool completed_finite_stream_ = false;
    bool awaiting_request_after_finite_stream_ = false;
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
    std::uint64_t generation_ = 0;
    bool measurement_active_ = false;
    bool reset_active_ = false;
#endif
  };

  void Emit() const;
  void ResetCounters() noexcept;

#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  std::mutex measurement_mutex;
  std::condition_variable measurement_ready;
  std::size_t active_measurements = 0;
  bool resetting = false;
  std::uint64_t generation = 0;
#endif
  std::atomic<std::uint64_t> accepted_unary_connections{0};
  std::atomic<std::uint64_t> completed_unary_operations{0};
  std::atomic<std::uint64_t> finite_stream_connections{0};
  std::atomic<std::uint64_t> completed_finite_streams{0};
  std::atomic<std::uint64_t> connections_reused_after_finite_stream{0};
};

bool PerformanceSutDiagnostics::Impl::ConnectionObserver::BeginRequest(const server::HttpServerRequest& request,
                                                                       server::HttpServerResponse& response) {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  const bool is_reset = request.method == core::http::kMethodPost && request.target == kDiagnosticsResetPath;
  {
    std::unique_lock lock(diagnostics_.measurement_mutex);
    diagnostics_.measurement_ready.wait(lock, [this] { return !diagnostics_.resetting; });
    if (is_reset) {
      diagnostics_.resetting = true;
      reset_active_ = true;
      diagnostics_.measurement_ready.wait(lock, [this] { return diagnostics_.active_measurements == 0; });
      diagnostics_.ResetCounters();
      (void)core::subscription_diagnostics::TakeSnapshot();
    } else {
      ++diagnostics_.active_measurements;
      measurement_active_ = true;
    }
  }
  SynchronizeGeneration();
  if (is_reset) {
    response.status_code = core::http::kStatusNoContent;
    return true;
  }
#else
  (void)request;
  (void)response;
#endif
  if (awaiting_request_after_finite_stream_) {
    diagnostics_.connections_reused_after_finite_stream.fetch_add(1, std::memory_order_relaxed);
    awaiting_request_after_finite_stream_ = false;
  }
  return false;
}

void PerformanceSutDiagnostics::Impl::ConnectionObserver::FinishRequest(const server::HttpServerResponse& response,
                                                                        bool close_connection,
                                                                        bool handled_by_observer) {
  if (!handled_by_observer) {
    if (!response.stream_writer) {
      if (!completed_unary_) {
        completed_unary_ = true;
        diagnostics_.accepted_unary_connections.fetch_add(1, std::memory_order_relaxed);
      }
      diagnostics_.completed_unary_operations.fetch_add(1, std::memory_order_relaxed);
    }
    if (response.stream_kind == server::HttpStreamKind::kFinite) {
      if (!completed_finite_stream_) {
        completed_finite_stream_ = true;
        diagnostics_.finite_stream_connections.fetch_add(1, std::memory_order_relaxed);
      }
      diagnostics_.completed_finite_streams.fetch_add(1, std::memory_order_relaxed);
      awaiting_request_after_finite_stream_ = !close_connection;
    }
  }
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  ReleaseMeasurement();
#endif
}

void PerformanceSutDiagnostics::Impl::ConnectionObserver::ReleaseMeasurement() {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  {
    std::lock_guard lock(diagnostics_.measurement_mutex);
    if (measurement_active_) {
      --diagnostics_.active_measurements;
      measurement_active_ = false;
    }
    if (reset_active_) {
      diagnostics_.resetting = false;
      reset_active_ = false;
    }
  }
  diagnostics_.measurement_ready.notify_all();
#endif
}

void PerformanceSutDiagnostics::Impl::ConnectionObserver::SynchronizeGeneration() {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  if (generation_ == diagnostics_.generation) {
    return;
  }
  generation_ = diagnostics_.generation;
  completed_unary_ = false;
  completed_finite_stream_ = false;
  awaiting_request_after_finite_stream_ = false;
#endif
}

void PerformanceSutDiagnostics::Impl::ResetCounters() noexcept {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  ++generation;
#endif
  accepted_unary_connections.store(0, std::memory_order_relaxed);
  completed_unary_operations.store(0, std::memory_order_relaxed);
  finite_stream_connections.store(0, std::memory_order_relaxed);
  completed_finite_streams.store(0, std::memory_order_relaxed);
  connections_reused_after_finite_stream.store(0, std::memory_order_relaxed);
}

void PerformanceSutDiagnostics::Impl::Emit() const {
  const std::uint64_t accepted = accepted_unary_connections.load(std::memory_order_relaxed);
  const std::uint64_t unary = completed_unary_operations.load(std::memory_order_relaxed);
  const std::uint64_t stream_connections = finite_stream_connections.load(std::memory_order_relaxed);
  const std::uint64_t streams = completed_finite_streams.load(std::memory_order_relaxed);
  const double operations_per_connection =
      accepted == 0U ? 0.0 : static_cast<double>(unary) / static_cast<double>(accepted);
  const double streams_per_connection =
      stream_connections == 0U ? 0.0 : static_cast<double>(streams) / static_cast<double>(stream_connections);
  std::cout << kHttpDiagnosticsPrefix << " accepted_connections=" << accepted << " completed_unary_operations=" << unary
            << " operations_per_connection=" << operations_per_connection
            << " finite_stream_connections=" << stream_connections << " completed_finite_streams=" << streams
            << " finite_streams_per_connection=" << streams_per_connection << " connections_reused_after_finite_stream="
            << connections_reused_after_finite_stream.load(std::memory_order_relaxed) << '\n';
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  if (core::subscription_diagnostics::IsEnabled()) {
    const auto snapshot = core::subscription_diagnostics::TakeSnapshot();
    std::cout << kSubscriptionDiagnosticsPrefix;
    for (std::size_t index = 0; index < snapshot.size(); ++index) {
      const auto& aggregate = snapshot[index];
      const auto phase_name = core::subscription_diagnostics::kPhaseNames[index];
      std::cout << ' ' << phase_name << "_count=" << aggregate.count << ' ' << phase_name
                << "_total_ns=" << aggregate.elapsed_nanoseconds << ' ' << phase_name
                << "_max_ns=" << aggregate.maximum_nanoseconds;
    }
    std::cout << '\n';
  }
#endif
  std::cout << std::flush;
}

PerformanceSutDiagnostics::PerformanceSutDiagnostics() : impl_(std::make_unique<Impl>()) {}
PerformanceSutDiagnostics::~PerformanceSutDiagnostics() = default;

std::unique_ptr<SutHttpConnectionObserver> PerformanceSutDiagnostics::ObserveHttpConnection() {
  return std::make_unique<Impl::ConnectionObserver>(*impl_);
}

bool PerformanceSutDiagnostics::IsHttpMeasurementReset(const server::HttpServerRequest& request) const {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  return request.method == core::http::kMethodPost && request.target == kDiagnosticsResetPath;
#else
  (void)request;
  return false;
#endif
}

void PerformanceSutDiagnostics::OnShutdown() { impl_->Emit(); }

}  // namespace a2a::tests::sut
