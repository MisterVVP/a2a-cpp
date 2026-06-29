// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::core {

struct AgentCardRequestContext final {
  std::optional<std::string> remote_address;
  std::unordered_map<std::string, std::string> client_headers;
  std::unordered_map<std::string, std::string> auth_metadata;
};

class AgentCardProvider {
 public:
  AgentCardProvider() = default;
  AgentCardProvider(const AgentCardProvider&) = delete;
  AgentCardProvider& operator=(const AgentCardProvider&) = delete;
  AgentCardProvider(AgentCardProvider&&) = delete;
  AgentCardProvider& operator=(AgentCardProvider&&) = delete;
  virtual ~AgentCardProvider() = default;

  [[nodiscard]] virtual Result<lf::a2a::v1::AgentCard> GetExtendedAgentCard(
      const AgentCardRequestContext& context) const = 0;
};

class StaticAgentCardProvider final : public AgentCardProvider {
 public:
  StaticAgentCardProvider() = default;
  explicit StaticAgentCardProvider(std::optional<lf::a2a::v1::AgentCard> extended_agent_card);

  [[nodiscard]] Result<lf::a2a::v1::AgentCard> GetExtendedAgentCard(
      const AgentCardRequestContext& context) const override;

 private:
  std::optional<lf::a2a::v1::AgentCard> extended_agent_card_;
};

}  // namespace a2a::core
