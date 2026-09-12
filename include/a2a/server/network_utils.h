// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

#include "a2a/core/core.h"

namespace a2a::server {

struct HostPortEndpoint final {
  std::string host;
  int port = 0;
};

void CloseSocketCrossPlatform(int fd) noexcept;

[[nodiscard]] std::string BuildHttpUrl(std::string_view host, int port, std::string_view path);
[[nodiscard]] a2a::core::Result<HostPortEndpoint> ParseHostPortEndpoint(std::string_view endpoint,
                                                                        int max_port = 65535);
[[nodiscard]] bool SetSocketNonBlocking(int fd) noexcept;
[[nodiscard]] bool SetSocketNoDelay(int fd) noexcept;

}  // namespace a2a::server
