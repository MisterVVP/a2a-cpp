// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/version.h"

namespace a2a::core {

std::string Version::HeaderValue() { return std::string(kProtocolVersion); }

bool Version::IsSupported(std::string_view version) noexcept { return version == kProtocolVersion; }

}  // namespace a2a::core
