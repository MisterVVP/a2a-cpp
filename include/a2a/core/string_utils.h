// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

namespace a2a::core::strings {

[[nodiscard]] std::string ToLowerAscii(std::string_view value);
[[nodiscard]] std::string_view TrimAsciiWhitespace(std::string_view value);
[[nodiscard]] bool EqualsAsciiCaseInsensitive(std::string_view lhs, std::string_view rhs);

}  // namespace a2a::core::strings
