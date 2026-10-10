// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>

#include "sut/sut_runtime.h"

namespace a2a::tests::sut {

class PerformanceSutDiagnostics final : public SutRuntimeObserver {
 public:
  PerformanceSutDiagnostics();
  ~PerformanceSutDiagnostics() override;

  PerformanceSutDiagnostics(const PerformanceSutDiagnostics&) = delete;
  PerformanceSutDiagnostics& operator=(const PerformanceSutDiagnostics&) = delete;
  PerformanceSutDiagnostics(PerformanceSutDiagnostics&&) = delete;
  PerformanceSutDiagnostics& operator=(PerformanceSutDiagnostics&&) = delete;

  [[nodiscard]] std::unique_ptr<SutHttpConnectionObserver> ObserveHttpConnection() override;
  [[nodiscard]] bool IsHttpMeasurementReset(const server::HttpServerRequest& request) const override;
  void OnShutdown() override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace a2a::tests::sut
