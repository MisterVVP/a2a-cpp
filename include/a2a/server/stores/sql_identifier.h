// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

namespace a2a::server::stores {

[[nodiscard]] bool IsAlphaOrUnderscore(unsigned char ch) noexcept;
[[nodiscard]] bool IsAlnumOrUnderscore(unsigned char ch) noexcept;
[[nodiscard]] bool IsValidSqlIdentifier(std::string_view identifier);
[[nodiscard]] std::string QuoteSqlIdentifier(std::string_view identifier);
[[nodiscard]] std::string QualifiedSqlIdentifier(std::string_view schema, std::string_view identifier);

}  // namespace a2a::server::stores
