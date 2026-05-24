// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

namespace a2a::server {

template <typename... Ts>
inline std::string Concat(Ts&&... ts) {
  const std::size_t size = (std::string_view{ts}.size() + ...);
  std::string result;
  result.reserve(size);
  (result.append(ts), ...);
  return result;
}

}  // namespace a2a::server
