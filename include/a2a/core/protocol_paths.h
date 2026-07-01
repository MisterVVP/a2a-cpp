// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string_view>

namespace a2a::core::protocol_paths {

inline constexpr std::string_view kAgentCard = "/.well-known/agent-card.json";
inline constexpr std::string_view kLegacyAgentCard = "/.well-known/agent.json";
inline constexpr std::string_view kExtendedAgentCard = "/extendedAgentCard";
inline constexpr std::string_view kWellKnownPrefix = "/.well-known/";

}  // namespace a2a::core::protocol_paths
