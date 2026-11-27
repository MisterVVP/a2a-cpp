// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <exception>
#include <iostream>

#include "sut/sut_runtime.h"

int main(int argc, char** argv) noexcept {
  try {
    return a2a::tests::sut::RunSutRuntime(argc, argv, {.display_name = "Async TCK SUT", .asynchronous = true});
  } catch (const std::exception& error) {
    std::cerr << "Unhandled async TCK SUT exception: " << error.what() << '\n';
    return 1;
  }
}
