#include "a2a/server/rest_adapter.h"

#include <gtest/gtest.h>

#include <string_view>

#include "a2a/core/protojson.h"

namespace {

constexpr std::string_view kWellKnownPath = "/.well-known/agent-card.json";

TEST(RestAdapterTest, PublishesAgentCardWithRequiredHeadersAndRestInterface) {
  a2a::server::RestAdapter adapter({
      .rest_base_url = "https://agent.example.com/a2a/",
      .agent_card_provider = [](const a2a::server::RequestContext&) {
        lf::a2a::v1::AgentCard card;
        card.set_name("Example Agent");
        card.add_default_security_requirements("oauth2");
        (*card.mutable_security_schemes())["oauth2"].set_type("oauth2");
        return a2a::core::Result<lf::a2a::v1::AgentCard>(card);
      },
  });

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = std::string(kWellKnownPath);
  const auto response = adapter.Handle(request, {});

  ASSERT_EQ(response.status_code, 200);
  ASSERT_TRUE(response.headers.contains("Content-Type"));
  EXPECT_EQ(response.headers.at("Content-Type"), "application/json");
  ASSERT_TRUE(response.headers.contains("A2A-Version"));
  EXPECT_EQ(response.headers.at("A2A-Version"), "1.0");

  lf::a2a::v1::AgentCard card;
  const auto parse = a2a::core::JsonToMessage(response.body, &card);
  ASSERT_TRUE(parse.ok()) << parse.error().message();
  EXPECT_EQ(card.protocol_version(), "1.0");

  bool has_rest_endpoint = false;
  for (const auto& iface : card.supported_interfaces()) {
    if (iface.transport() == lf::a2a::v1::TRANSPORT_PROTOCOL_REST &&
        iface.url() == "https://agent.example.com/a2a") {
      has_rest_endpoint = true;
      EXPECT_EQ(iface.security_requirements_size(), 1);
      EXPECT_EQ(iface.security_requirements(0), "oauth2");
    }
  }
  EXPECT_TRUE(has_rest_endpoint);
}

TEST(RestAdapterTest, FiltersUndefinedSecurityRequirements) {
  a2a::server::RestAdapter adapter({
      .rest_base_url = "https://agent.example.com/a2a",
      .agent_card_provider = [](const a2a::server::RequestContext&) {
        lf::a2a::v1::AgentCard card;
        card.add_default_security_requirements("oauth2");
        card.add_default_security_requirements("missing-default");
        (*card.mutable_security_schemes())["oauth2"].set_type("oauth2");
        auto* rest = card.add_supported_interfaces();
        rest->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_REST);
        rest->set_url("https://agent.example.com/another");
        rest->add_security_requirements("oauth2");
        rest->add_security_requirements("missing-interface");
        return a2a::core::Result<lf::a2a::v1::AgentCard>(card);
      },
  });

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = std::string(kWellKnownPath);
  const auto response = adapter.Handle(request, {});

  ASSERT_EQ(response.status_code, 200);
  lf::a2a::v1::AgentCard card;
  const auto parse = a2a::core::JsonToMessage(response.body, &card);
  ASSERT_TRUE(parse.ok()) << parse.error().message();

  ASSERT_EQ(card.default_security_requirements_size(), 1);
  EXPECT_EQ(card.default_security_requirements(0), "oauth2");
  for (const auto& iface : card.supported_interfaces()) {
    for (const auto& requirement : iface.security_requirements()) {
      EXPECT_TRUE(card.security_schemes().contains(requirement));
    }
  }
}

TEST(RestAdapterTest, ReturnsNotFoundForUnknownRoute) {
  a2a::server::RestAdapter adapter({
      .rest_base_url = "https://agent.example.com/a2a",
      .agent_card_provider = [](const a2a::server::RequestContext&) {
        return a2a::core::Result<lf::a2a::v1::AgentCard>(lf::a2a::v1::AgentCard{});
      },
  });

  a2a::server::RestRequest request;
  request.method = "GET";
  request.path = "/unknown";
  const auto response = adapter.Handle(request, {});
  EXPECT_EQ(response.status_code, 404);
}

}  // namespace
