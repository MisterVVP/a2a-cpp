// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

#include "sut/sut_runtime.h"

namespace {

constexpr auto kAsyncGrpcEnv = "A2A_TCK_GRPC_ASYNC";
constexpr auto kEnabled = "1";
constexpr std::string_view kTckSutName = "TCK SUT";

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    const char* mode = std::getenv(kAsyncGrpcEnv);
    const bool asynchronous = mode != nullptr && std::string_view(mode) == kEnabled;
    return a2a::tests::sut::RunSutRuntime(argc, argv, {.display_name = kTckSutName, .async_grpc = asynchronous});
  } catch (const std::exception& ex) {
    std::cerr << "Unhandled TCK SUT exception: " << ex.what() << '\n';
    return 1;
  }
}
