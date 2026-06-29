// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/agent_card/agent_card_provider.h"

#include <optional>
#include <utility>

#include "a2a/core/protocol_errors.h"

namespace a2a::core {

StaticAgentCardProvider::StaticAgentCardProvider(std::optional<lf::a2a::v1::AgentCard> extended_agent_card)
    : extended_agent_card_(std::move(extended_agent_card)) {}

Result<lf::a2a::v1::AgentCard> StaticAgentCardProvider::GetExtendedAgentCard(
    const AgentCardRequestContext& context) const {
  (void)context;
  if (!extended_agent_card_.has_value()) {
    return protocol_errors::ExtendedAgentCardNotConfigured();
  }
  return *extended_agent_card_;
}

}  // namespace a2a::core
