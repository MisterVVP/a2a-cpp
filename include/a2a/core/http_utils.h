// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace a2a::core::http {

[[nodiscard]] std::optional<std::string_view> FindHeaderValue(
    const std::unordered_map<std::string, std::string>& headers, std::string_view name);
[[nodiscard]] bool IsJsonContentType(std::string_view content_type);

}  // namespace a2a::core::http
