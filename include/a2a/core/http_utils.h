// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "a2a/core/string_utils.h"

namespace a2a::core::http {

[[nodiscard]] std::optional<std::string_view> FindHeaderValue(
    const std::unordered_map<std::string, std::string>& headers, std::string_view name);

// Supports ordered header containers (including a2a::http::Response::headers)
// whose elements expose `name` and `value` string members.
template <typename HeaderContainer>
[[nodiscard]] std::optional<std::string_view> FindHeaderValue(const HeaderContainer& headers, std::string_view name) {
  for (const auto& header : headers) {
    if (strings::EqualsAsciiCaseInsensitive(header.name, name)) {
      return std::string_view(header.value);
    }
  }
  return std::nullopt;
}
[[nodiscard]] bool IsMediaType(std::string_view content_type, std::string_view expected_media_type);
[[nodiscard]] bool IsJsonContentType(std::string_view content_type);
[[nodiscard]] bool IsSseContentType(std::string_view content_type);

}  // namespace a2a::core::http
