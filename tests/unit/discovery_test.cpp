// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/discovery.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "a2a/core/error.h"

namespace {

using a2a::client::AgentCardResolver;
using a2a::client::DiscoveryClient;
using a2a::client::HttpResponse;
using a2a::client::PreferredTransport;

constexpr int kHttpNotFound = 404;
constexpr int kHttpOk = 200;

TEST(DiscoveryClientTest, RejectsMalformedBaseUrl) {
  DiscoveryClient client([](std::string_view) -> a2a::core::Result<HttpResponse> { return HttpResponse{}; });

  const auto result = client.Fetch("ftp://example.com");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(DiscoveryClientTest, MapsWellKnownNotFoundToRemoteProtocolError) {
  DiscoveryClient client([](std::string_view) -> a2a::core::Result<HttpResponse> {
    return HttpResponse{.status_code = kHttpNotFound, .body = "missing"};
  });

  const auto result = client.Fetch("https://agent.example.com");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
  const auto& http_status = result.error().http_status();
  ASSERT_TRUE(http_status.has_value());
  EXPECT_EQ(http_status.value_or(-1), kHttpNotFound);
}

TEST(DiscoveryClientTest, ReportsBadJsonWithSerializationError) {
  DiscoveryClient client([](std::string_view) -> a2a::core::Result<HttpResponse> {
    return HttpResponse{.status_code = kHttpOk, .body = "{not-json"};
  });

  const auto result = client.Fetch("https://agent.example.com");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kSerialization);
}

TEST(DiscoveryClientTest, RejectsCardsWithoutSupportedInterfaces) {
  DiscoveryClient client([](std::string_view) -> a2a::core::Result<HttpResponse> {
    return HttpResponse{.status_code = kHttpOk, .body = R"({"name":"no-interfaces"})"};
  });

  const auto result = client.Fetch("https://agent.example.com");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(DiscoveryClientTest, RejectsUnsupportedProtocolVersion) {
  DiscoveryClient client([](std::string_view) -> a2a::core::Result<HttpResponse> {
    return HttpResponse{
        .status_code = kHttpOk,
        .body =
            R"({"supportedInterfaces":[{"protocolBinding":"HTTP+JSON","protocolVersion":"2.0","url":"https://agent.example.com/a2a"}]})"};
  });

  const auto result = client.Fetch("https://agent.example.com");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kUnsupportedVersion);
}

TEST(DiscoveryClientTest, RejectsUnknownSecurityRequirementReferences) {
  DiscoveryClient client([](std::string_view) -> a2a::core::Result<HttpResponse> {
    return HttpResponse{
        .status_code = kHttpOk,
        .body =
            R"({"supportedInterfaces":[{"protocolBinding":"HTTP+JSON","protocolVersion":"1.0","url":"https://agent.example.com/a2a"}],"securityRequirements":[{"schemes":{"oauth2":{"list":[]}}}]})"};
  });

  const auto result = client.Fetch("https://agent.example.com");
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(DiscoveryClientTest, UsesInMemoryCacheWithinTtl) {
  std::size_t calls = 0;
  DiscoveryClient client(
      [&calls](std::string_view) -> a2a::core::Result<HttpResponse> {
        ++calls;
        return HttpResponse{
            .status_code = kHttpOk,
            .body =
                R"({"supportedInterfaces":[{"protocolBinding":"HTTP+JSON","protocolVersion":"1.0","url":"https://agent.example.com/a2a"}]})"};
      },
      a2a::client::kDefaultDiscoveryCacheTtl);

  const auto first = client.Fetch("https://agent.example.com/");
  ASSERT_TRUE(first.ok()) << first.error().message();
  const auto second = client.Fetch("https://agent.example.com");
  ASSERT_TRUE(second.ok()) << second.error().message();
  EXPECT_EQ(calls, 1U);
}

TEST(DiscoveryClientTest, FetchExtendedAgentCardUsesSpecEndpoint) {
  std::string called_url;
  DiscoveryClient client([&called_url](std::string_view url) -> a2a::core::Result<HttpResponse> {
    called_url = std::string(url);
    return HttpResponse{
        .status_code = kHttpOk,
        .body =
            R"({"supportedInterfaces":[{"protocolBinding":"HTTP+JSON","protocolVersion":"1.0","url":"https://agent.example.com/a2a"}]})"};
  });

  const auto fetched = client.FetchExtendedAgentCard("https://agent.example.com/");
  ASSERT_TRUE(fetched.ok()) << fetched.error().message();
  EXPECT_EQ(called_url, "https://agent.example.com/extendedAgentCard");
}

TEST(AgentCardResolverTest, SelectsPreferredThenFallsBack) {
  lf::a2a::v1::AgentCard card;
  auto* json_rpc = card.add_supported_interfaces();
  json_rpc->set_protocol_binding("JSONRPC");
  json_rpc->set_protocol_version("1.0");
  json_rpc->set_url("https://agent.example.com/rpc");
  auto* grpc = card.add_supported_interfaces();
  grpc->set_protocol_binding("GRPC");
  grpc->set_protocol_version("1.0");
  grpc->set_url("https://agent.example.com/grpc");

  const auto resolved = AgentCardResolver::SelectPreferredInterface(card, PreferredTransport::kRest);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message();
  EXPECT_EQ(resolved.value().transport, PreferredTransport::kJsonRpc);
  EXPECT_EQ(resolved.value().url, "https://agent.example.com/rpc");
}

TEST(AgentCardResolverTest, SelectsGrpcInterfaceWhenPreferred) {
  lf::a2a::v1::AgentCard card;
  auto* grpc = card.add_supported_interfaces();
  grpc->set_protocol_binding("GRPC");
  grpc->set_protocol_version("1.0");
  grpc->set_url("dns:///agent.example.com:50051");

  const auto resolved = AgentCardResolver::SelectPreferredInterface(card, PreferredTransport::kGrpc);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message();
  EXPECT_EQ(resolved.value().transport, PreferredTransport::kGrpc);
  EXPECT_EQ(resolved.value().url, "dns:///agent.example.com:50051");
}

TEST(AgentCardResolverTest, UsesDefaultSecurityRequirementsWhenInterfaceSpecificNotSet) {
  lf::a2a::v1::AgentCard card;
  (*card.mutable_security_schemes())["oauth2"].mutable_oauth2_security_scheme();
  auto* requirement = card.add_security_requirements();
  (*requirement->mutable_schemes())["oauth2"];
  auto* rest = card.add_supported_interfaces();
  rest->set_protocol_binding("HTTP+JSON");
  rest->set_protocol_version("1.0");
  rest->set_url("https://agent.example.com/a2a");

  const auto resolved = AgentCardResolver::SelectPreferredInterface(card, PreferredTransport::kRest);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message();
  ASSERT_EQ(resolved.value().security_requirements.size(), 1U);
  EXPECT_EQ(resolved.value().security_requirements[0], "oauth2");
  EXPECT_TRUE(resolved.value().security_schemes.contains("oauth2"));
}

TEST(AgentCardResolverTest, ReturnsValidationErrorWhenNoUsableInterfaceExists) {
  lf::a2a::v1::AgentCard card;
  auto* iface = card.add_supported_interfaces();
  iface->set_protocol_binding("");
  iface->set_url("");

  const auto resolved = AgentCardResolver::SelectPreferredInterface(card, PreferredTransport::kRest);
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(DiscoveryIntegrationFixtureTest, LoadsValidFixtureAndResolvesSecurityMetadata) {
  std::ifstream fixture(std::string(A2A_SOURCE_DIR) + "/tests/fixtures/agent_card_valid.json");
  ASSERT_TRUE(fixture.is_open());
  std::string json((std::istreambuf_iterator<char>(fixture)), std::istreambuf_iterator<char>());

  DiscoveryClient client([json](std::string_view) -> a2a::core::Result<HttpResponse> {
    return HttpResponse{.status_code = kHttpOk, .body = json};
  });
  const auto fetched = client.Fetch("https://agent.example.com");
  ASSERT_TRUE(fetched.ok()) << fetched.error().message();

  const auto resolved = AgentCardResolver::SelectPreferredInterface(fetched.value(), PreferredTransport::kRest);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message();
  EXPECT_EQ(resolved.value().url, "https://agent.example.com/a2a");
  EXPECT_TRUE(resolved.value().security_schemes.contains("oauth2"));
}

}  // namespace
