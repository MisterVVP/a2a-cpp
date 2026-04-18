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
  DiscoveryClient client(
      [](std::string_view) -> a2a::core::Result<HttpResponse> { return HttpResponse{}; });

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
    return HttpResponse{.status_code = kHttpOk,
                        .body = R"({"protocolVersion":"1.0","name":"no-interfaces"})"};
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
                R"({"protocolVersion":"1.0","supportedInterfaces":[{"transport":"TRANSPORT_PROTOCOL_REST","url":"https://agent.example.com/a2a"}]})"};
      },
      a2a::client::kDefaultDiscoveryCacheTtl);

  const auto first = client.Fetch("https://agent.example.com/");
  ASSERT_TRUE(first.ok()) << first.error().message();
  const auto second = client.Fetch("https://agent.example.com");
  ASSERT_TRUE(second.ok()) << second.error().message();
  EXPECT_EQ(calls, 1U);
}

TEST(AgentCardResolverTest, SelectsPreferredThenFallsBack) {
  lf::a2a::v1::AgentCard card;
  card.set_protocol_version("1.0");
  auto* json_rpc = card.add_supported_interfaces();
  json_rpc->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC);
  json_rpc->set_url("https://agent.example.com/rpc");
  auto* grpc = card.add_supported_interfaces();
  grpc->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_GRPC);
  grpc->set_url("https://agent.example.com/grpc");

  const auto resolved =
      AgentCardResolver::SelectPreferredInterface(card, PreferredTransport::kRest);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message();
  EXPECT_EQ(resolved.value().transport, PreferredTransport::kJsonRpc);
  EXPECT_EQ(resolved.value().url, "https://agent.example.com/rpc");
}

TEST(AgentCardResolverTest, ReturnsValidationErrorWhenNoUsableInterfaceExists) {
  lf::a2a::v1::AgentCard card;
  card.set_protocol_version("1.0");
  auto* iface = card.add_supported_interfaces();
  iface->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_UNSPECIFIED);
  iface->set_url("");

  const auto resolved =
      AgentCardResolver::SelectPreferredInterface(card, PreferredTransport::kRest);
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

  const auto resolved =
      AgentCardResolver::SelectPreferredInterface(fetched.value(), PreferredTransport::kRest);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message();
  EXPECT_EQ(resolved.value().url, "https://agent.example.com/a2a");
  EXPECT_TRUE(resolved.value().security_schemes.contains("oauth2"));
}

}  // namespace
