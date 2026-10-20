// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::core {

// Bound resolver and cryptographic work for untrusted discovery documents.
inline constexpr std::size_t kMaxAgentCardSignatures = 16;

struct AgentCardVerificationKey final {
  std::string public_key_pem;
  std::chrono::system_clock::time_point valid_from;
  std::chrono::system_clock::time_point valid_until;
  bool revoked = false;
};
using AgentCardKeyResolver = std::function<Result<AgentCardVerificationKey>(std::string_view key_id)>;

[[nodiscard]] Result<lf::a2a::v1::AgentCard> SignAgentCard(const lf::a2a::v1::AgentCard& card, std::string_view key_id,
                                                           std::string_view private_key_pem);
[[nodiscard]] Result<void> VerifyAgentCardJson(std::string_view json, const AgentCardKeyResolver& resolver,
                                               std::chrono::system_clock::time_point now);

}  // namespace a2a::core
