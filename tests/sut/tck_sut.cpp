// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <exception>
#include <iostream>

#include "sut/sut_runtime.h"

namespace {

constexpr std::string_view kTckSutName = "TCK SUT";

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    return a2a::tests::sut::RunSutRuntime(argc, argv, {.display_name = kTckSutName});
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled TCK SUT exception: " << ex.what() << '\n';
    return 1;
  }
}
