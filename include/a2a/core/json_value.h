// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

namespace a2a::core::json {

struct ValueRange final {
  std::size_t begin;
  std::size_t end;
};

// Locates an unescaped member name in the outermost JSON object. The returned
// half-open range refers to the member's complete JSON value in `json`.
// Returns nullopt when the document is malformed, the member is absent, or the
// member occurs more than once.
[[nodiscard]] std::optional<ValueRange> FindTopLevelObjectMemberValue(std::string_view json,
                                                                      std::string_view member_name) noexcept;

}  // namespace a2a::core::json
