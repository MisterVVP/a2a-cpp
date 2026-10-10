// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <exception>
#include <iostream>
#include <string_view>

#include "sut/performance_sut_diagnostics.h"
#include "sut/sut_runtime.h"

namespace {

constexpr std::string_view kPerformanceSutName = "Performance SUT";

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    a2a::tests::sut::PerformanceSutDiagnostics diagnostics;
    return a2a::tests::sut::RunSutRuntime(
        argc, argv,
        {.display_name = kPerformanceSutName,
         .observer = &diagnostics,
         .http_implementation = a2a::tests::sut::SutHttpImplementation::kAsynchronous});
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled performance SUT exception: " << ex.what() << '\n';
    return 1;
  }
}
