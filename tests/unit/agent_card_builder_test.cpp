// SPDX-License-Identifier: Apache-2.0

#include "a2a/core/agent_card_builder.h"

#include <gtest/gtest.h>

#include "a2a/core/protocol_bindings.h"
#include "a2a/core/version.h"

namespace {

constexpr std::string_view kVersion = a2a::core::Version::kAgentCardVersion;
constexpr std::string_view kDescription = "desc";
constexpr std::string_view kName = "agent";
constexpr std::string_view kProtocolVersion = "1.0";
constexpr std::string_view kHttpJsonUrl = "http://agent.local/a2a";
constexpr std::string_view kTckRequiredExtensionUri = "urn:a2a:tck:required-extension";

TEST(AgentCardBuilderTest, RestPresetBuildsExpectedInterface) {
  const auto card = a2a::core::AgentCardBuilder::RestPreset("REST Agent", kHttpJsonUrl).Build();

  ASSERT_EQ(card.supported_interfaces_size(), 1);
  EXPECT_EQ(card.name(), "REST Agent");
  EXPECT_EQ(card.supported_interfaces(0).protocol_binding(), a2a::core::protocol_bindings::kHttpJson);
}

TEST(AgentCardBuilderTest, PresetsValidateSuccessfully) {
  EXPECT_TRUE(a2a::core::AgentCardBuilder::RestPreset("r", "https://agent.local/a2a").Validate().ok());
  EXPECT_TRUE(a2a::core::AgentCardBuilder::JsonRpcPreset("j", "http://agent.local/rpc").Validate().ok());
  EXPECT_TRUE(a2a::core::AgentCardBuilder::GrpcPreset("g", "dns:///agent.local:50051").Validate().ok());
}

TEST(AgentCardBuilderTest, ConformancePresetBuildsExpectedDefaults) {
  const auto card = a2a::core::AgentCardBuilder::ConformancePreset({.rest_url = kHttpJsonUrl,
                                                                    .json_rpc_url = "http://agent.local/rpc",
                                                                    .grpc_url = "agent.local:50051"})
                        .Build();

  EXPECT_EQ(card.supported_interfaces_size(), 3);
  EXPECT_TRUE(card.capabilities().streaming());
  EXPECT_FALSE(card.capabilities().push_notifications());
  ASSERT_EQ(card.capabilities().extensions_size(), 1);
  EXPECT_EQ(card.capabilities().extensions(0).uri(), kTckRequiredExtensionUri);
  EXPECT_TRUE(card.capabilities().extensions(0).required());
  ASSERT_EQ(card.skills_size(), 1);
  EXPECT_EQ(card.skills(0).id(), "echo");
  EXPECT_EQ(card.skills(0).tags_size(), 1);
  EXPECT_EQ(card.skills(0).tags(0), "conformance");
}

TEST(AgentCardBuilderTest, ValidateRejectsMissingRequiredFields) {
  EXPECT_FALSE(a2a::core::AgentCardBuilder().Validate().ok());
  EXPECT_FALSE(a2a::core::AgentCardBuilder().SetName(kName).Validate().ok());
  EXPECT_FALSE(a2a::core::AgentCardBuilder().SetName(kName).SetVersion(kVersion).Validate().ok());
}

TEST(AgentCardBuilderTest, ValidateRejectsInterfaceWithoutVersion) {
  auto builder =
      a2a::core::AgentCardBuilder()
          .SetName(kName)
          .SetVersion(kVersion)
          .SetDescription(kDescription)
          .AddInterface({.binding = a2a::core::protocol_bindings::kHttpJson, .version = "", .url = kHttpJsonUrl});

  EXPECT_FALSE(builder.Validate().ok());
}

TEST(AgentCardBuilderTest, ValidateRejectsExtensionWithoutUri) {
  auto builder = a2a::core::AgentCardBuilder()
                     .SetName(kName)
                     .SetVersion(kVersion)
                     .SetDescription(kDescription)
                     .AddInterface({.binding = a2a::core::protocol_bindings::kHttpJson,
                                    .version = kProtocolVersion,
                                    .url = kHttpJsonUrl})
                     .AddExtension("", true);

  EXPECT_FALSE(builder.Validate().ok());
}

TEST(AgentCardBuilderTest, ValidateRejectsDuplicateInterfaces) {
  auto builder = a2a::core::AgentCardBuilder()
                     .SetName("dup")
                     .SetVersion(kVersion)
                     .SetDescription(kDescription)
                     .AddInterface({.binding = a2a::core::protocol_bindings::kHttpJson,
                                    .version = kProtocolVersion,
                                    .url = kHttpJsonUrl})
                     .AddInterface({.binding = a2a::core::protocol_bindings::kHttpJson,
                                    .version = kProtocolVersion,
                                    .url = kHttpJsonUrl});

  EXPECT_FALSE(builder.Validate().ok());
}

TEST(AgentCardBuilderTest, ValidateRejectsInvalidUrl) {
  auto builder =
      a2a::core::AgentCardBuilder()
          .SetName("invalid")
          .SetVersion(kVersion)
          .SetDescription(kDescription)
          .AddInterface(
              {.binding = a2a::core::protocol_bindings::kJsonRpc, .version = kProtocolVersion, .url = "localhost:8080/rpc"});

  EXPECT_FALSE(builder.Validate().ok());
}

TEST(AgentCardBuilderTest, ValidateAcceptsGrpcHostPortUrl) {
  auto builder = a2a::core::AgentCardBuilder()
                     .SetName("grpc")
                     .SetVersion(kVersion)
                     .SetDescription(kDescription)
                     .AddInterface({.binding = a2a::core::protocol_bindings::kGrpc,
                                    .version = kProtocolVersion,
                                    .url = "localhost:50051"});

  EXPECT_TRUE(builder.Validate().ok());
}

TEST(AgentCardBuilderTest, WithPushNotificationsPreservesExistingCapabilities) {
  const auto card = a2a::core::AgentCardBuilder::ConformancePreset({.rest_url = kHttpJsonUrl,
                                                                    .json_rpc_url = "http://agent.local/rpc",
                                                                    .grpc_url = "agent.local:50051"})
                        .WithPushNotifications(true)
                        .Build();

  EXPECT_TRUE(card.capabilities().streaming());
  EXPECT_TRUE(card.capabilities().push_notifications());
}

}  // namespace
