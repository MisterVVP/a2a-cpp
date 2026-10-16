// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

#include "a2a/core/result.h"

namespace a2a::core {
[[nodiscard]] Result<std::string> CanonicalizeJson(std::string_view json);
}  // namespace a2a::core
