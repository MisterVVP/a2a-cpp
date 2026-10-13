// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace a2a::core::internal {
[[nodiscard]] nlohmann::json ParseSigningJson(std::string_view input);
[[nodiscard]] std::string Canonicalize(const nlohmann::json& value);
}  // namespace a2a::core::internal
