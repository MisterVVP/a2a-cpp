// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <exception>
#include <iostream>
#include <string_view>

#include "sut/sut_runtime.h"

namespace {
constexpr std::string_view kDisplayName = "HTTP I/O test SUT";
}

int main(int argc, char** argv) noexcept {
  try {
    return a2a::tests::sut::RunSutRuntime(argc, argv,
                                          {.display_name = kDisplayName,
                                           .http_implementation = a2a::tests::sut::SutHttpImplementation::kAsynchronous,
                                           .enable_grpc = false});
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
