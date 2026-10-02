// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string_view>

namespace a2a::tests::sut {

struct SutRuntimeOptions final {
  std::string_view display_name;
  bool enable_http_diagnostics = false;
  void (*emit_shutdown_diagnostics)() = nullptr;
};

int RunSutRuntime(int argc, char** argv, const SutRuntimeOptions& options);

}  // namespace a2a::tests::sut
