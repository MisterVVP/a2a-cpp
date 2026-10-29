// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <exception>
#include <iostream>

#include "sut/async_sut_runtime.h"

int main(int argc, char** argv) noexcept {
  try {
    return a2a::tests::sut::RunAsyncSutRuntime(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "Unhandled async TCK SUT exception: " << error.what() << '\n';
    return 1;
  }
}
