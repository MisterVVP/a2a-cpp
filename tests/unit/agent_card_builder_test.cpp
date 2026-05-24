// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "a2a/core/agent_card_builder.h"
#include "a2a/core/protocol_bindings.h"

namespace {

TEST(AgentCardBuilderTest, RestPresetBuildsExpectedInterface) {
  const auto card = a2a::core::AgentCardBuilder::RestPreset("REST Agent", "http://agent.local/a2a").Build();

  ASSERT_EQ(card.supported_interfaces_size(), 1);
  EXPECT_EQ(card.name(), "REST Agent");
  EXPECT_EQ(card.supported_interfaces(0).protocol_binding(), a2a::core::protocol_bindings::kHttpJson);
}

TEST(AgentCardBuilderTest, ValidateRejectsDuplicateInterfaces) {
  auto builder = a2a::core::AgentCardBuilder()
                     .SetName("dup")
                     .SetVersion("1.0.0")
                     .SetDescription("desc")
                     .AddInterface(a2a::core::protocol_bindings::kHttpJson, "1.0", "http://agent.local/a2a")
                     .AddInterface(a2a::core::protocol_bindings::kHttpJson, "1.0", "http://agent.local/a2a");

  const auto result = builder.Validate();
  EXPECT_FALSE(result.ok());
}

TEST(AgentCardBuilderTest, ValidateRejectsInvalidUrl) {
  auto builder = a2a::core::AgentCardBuilder()
                     .SetName("invalid")
                     .SetVersion("1.0.0")
                     .SetDescription("desc")
                     .AddInterface(a2a::core::protocol_bindings::kJsonRpc, "1.0", "localhost:8080/rpc");

  const auto result = builder.Validate();
  EXPECT_FALSE(result.ok());
}

}  // namespace
