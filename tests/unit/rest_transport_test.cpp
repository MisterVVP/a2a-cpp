#include "a2a/server/rest_transport.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "a2a/core/protojson.h"

namespace {

constexpr std::string_view kWellKnownPath = "/.well-known/agent-card.json";
constexpr std::string_view kRestBaseUrlWithTrailingSlash = "https://agent.example.com/a2a/";
constexpr std::string_view kRestBaseUrl = "https://agent.example.com/a2a";
constexpr std::string_view kExpectedSecurityRequirement = "oauth2";

struct ParsedAgentCardResponse final {
  a2a::server::RestResponse response;
  lf::a2a::v1::AgentCard card;
};

a2a::server::RestRequest MakeGetRequest(std::string path) {
  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = std::move(path);
  return request;
}

a2a::server::RestTransport MakeAdapterWithDefaults(std::string rest_base_url) {
  return a2a::server::RestTransport({
      .rest_base_url = std::move(rest_base_url),
      .agent_card_provider =
          [](const a2a::server::RequestContext&) {
            lf::a2a::v1::AgentCard card;
            card.set_name("Example Agent");
            card.add_default_security_requirements(std::string(kExpectedSecurityRequirement));
            (*card.mutable_security_schemes())[std::string(kExpectedSecurityRequirement)].set_type(
                "oauth2");
            return a2a::core::Result<lf::a2a::v1::AgentCard>(card);
          },
  });
}

ParsedAgentCardResponse GetAndParseAgentCard(a2a::server::RestTransport& adapter) {
  ParsedAgentCardResponse parsed;
  parsed.response =
      adapter.Handle(MakeGetRequest(std::string(kWellKnownPath)), a2a::server::RequestContext{});

  const auto parse = a2a::core::JsonToMessage(parsed.response.body, &parsed.card);
  EXPECT_TRUE(parse.ok()) << parse.error().message();
  return parsed;
}

const lf::a2a::v1::AgentInterface* FindRestInterface(const lf::a2a::v1::AgentCard& card,
                                                     std::string_view expected_url) {
  for (const auto& iface : card.supported_interfaces()) {
    if (iface.transport() == lf::a2a::v1::TRANSPORT_PROTOCOL_REST && iface.url() == expected_url) {
      return &iface;
    }
  }
  return nullptr;
}

void ExpectSuccessHeaders(const a2a::server::RestResponse& response) {
  EXPECT_EQ(response.status_code, 200);
  ASSERT_TRUE(response.headers.contains("Content-Type"));
  EXPECT_EQ(response.headers.at("Content-Type"), "application/json");
  ASSERT_TRUE(response.headers.contains("A2A-Version"));
  EXPECT_EQ(response.headers.at("A2A-Version"), "1.0");
}

void ExpectProtocolAndRestInterface(const lf::a2a::v1::AgentCard& card) {
  EXPECT_EQ(card.protocol_version(), "1.0");
  const auto* rest_iface = FindRestInterface(card, kRestBaseUrl);
  ASSERT_NE(rest_iface, nullptr);
  ASSERT_EQ(rest_iface->security_requirements_size(), 1);
  EXPECT_EQ(rest_iface->security_requirements(0), kExpectedSecurityRequirement);
}

TEST(RestTransportTest, PublishesAgentCardWithRequiredHeadersAndRestInterface) {
  auto adapter = MakeAdapterWithDefaults(std::string(kRestBaseUrlWithTrailingSlash));

  const auto parsed = GetAndParseAgentCard(adapter);

  ExpectSuccessHeaders(parsed.response);
  ExpectProtocolAndRestInterface(parsed.card);
}

TEST(RestTransportTest, FiltersUndefinedSecurityRequirements) {
  a2a::server::RestTransport adapter({
      .rest_base_url = std::string(kRestBaseUrl),
      .agent_card_provider =
          [](const a2a::server::RequestContext&) {
            lf::a2a::v1::AgentCard card;
            card.add_default_security_requirements(std::string(kExpectedSecurityRequirement));
            card.add_default_security_requirements("missing-default");
            (*card.mutable_security_schemes())[std::string(kExpectedSecurityRequirement)].set_type(
                "oauth2");

            auto* rest = card.add_supported_interfaces();
            rest->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_REST);
            rest->set_url("https://agent.example.com/another");
            rest->add_security_requirements(std::string(kExpectedSecurityRequirement));
            rest->add_security_requirements("missing-interface");

            return a2a::core::Result<lf::a2a::v1::AgentCard>(card);
          },
  });

  const auto parsed = GetAndParseAgentCard(adapter);

  ASSERT_EQ(parsed.card.default_security_requirements_size(), 1);
  EXPECT_EQ(parsed.card.default_security_requirements(0), kExpectedSecurityRequirement);
  for (const auto& iface : parsed.card.supported_interfaces()) {
    for (const auto& requirement : iface.security_requirements()) {
      EXPECT_TRUE(parsed.card.security_schemes().contains(requirement));
    }
  }
}

TEST(RestTransportTest, ReturnsNotFoundForUnknownRoute) {
  auto adapter = MakeAdapterWithDefaults(std::string(kRestBaseUrl));

  const auto response = adapter.Handle(MakeGetRequest("/unknown"), a2a::server::RequestContext{});
  EXPECT_EQ(response.status_code, 404);
}

}  // namespace
