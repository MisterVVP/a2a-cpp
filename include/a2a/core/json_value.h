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
//
// This is a structural range scanner, not a standalone JSON validator.
// It validates the outer object framing, scalar values at the outer level,
// quoted-string boundaries, balanced object/array delimiters, duplicate target
// members, and the configured nesting limit. Nested composite payload grammar
// is intentionally not parsed because hot-path callers validate the extracted
// value separately.
//
// Callers handling untrusted JSON must validate both the surrounding document
// and the extracted value before using their contents.
//
// Returns nullopt when the outer structure is malformed, the member is absent,
// the member occurs more than once, or the nesting limit is exceeded.
[[nodiscard]] std::optional<ValueRange> FindTopLevelObjectMemberValue(std::string_view json,
                                                                      std::string_view member_name) noexcept;

}  // namespace a2a::core::json