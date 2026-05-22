#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/protojson.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "example_support.h"

namespace {

constexpr int kInternalServerErrorStatusCode = 500;

a2a::server::RestServerTransportOptions RestOptions(std::string rest_api_base_path) {
  return {.rest_api_base_path = std::move(rest_api_base_path),
          .require_version_header = true,
          .include_legacy_transport_fields = true,
          .agent_card_cache_settings = std::nullopt};
}

TEST(ExamplesInteropIntegrationTest, RestExampleServerRoundTripWorksViaDiscovery) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher, a2a::examples::BuildRestAgentCard("Interop Example Agent", "http://agent.local/a2a"),
      RestOptions("/a2a"));

  a2a::client::DiscoveryClient discovery([&server](std::string_view url) {
    const auto response = server.Handle(
        {.method = "GET", .target = a2a::examples::UrlToTarget(url), .headers = {}, .body = {}, .remote_address = {}});
    if (!response.ok()) {
      return a2a::client::HttpResponse{.status_code = kInternalServerErrorStatusCode, .body = {}};
    }
    return a2a::client::HttpResponse{.status_code = response.value().status_code, .body = response.value().body};
  });

  const auto card = discovery.Fetch("http://agent.local");
  ASSERT_TRUE(card.ok());
  const auto resolved =
      a2a::client::AgentCardResolver::SelectPreferredInterface(card.value(), a2a::client::PreferredTransport::kRest);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message();

  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      resolved.value(), [&server](const a2a::client::HttpRequest& request) {
        const auto response = server.Handle({.method = request.method,
                                             .target = a2a::examples::UrlToTarget(request.url),
                                             .headers = request.headers,
                                             .body = request.body,
                                             .remote_address = {}});
        if (!response.ok()) {
          return a2a::core::Result<a2a::client::HttpClientResponse>(response.error());
        }
        return a2a::core::Result<a2a::client::HttpClientResponse>(a2a::client::HttpClientResponse{
            .status_code = response.value().status_code,
            .headers = response.value().headers,
            .body = response.value().body,
        });
      });

  a2a::client::A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_message_id("interop-rest-msg-1");
  send.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  send.mutable_message()->add_parts()->set_text("hello");
  const auto send_result = client.SendMessage(send);
  ASSERT_TRUE(send_result.ok()) << send_result.error().message();
  EXPECT_FALSE(send_result.value().task().id().empty());
}

TEST(ExamplesInteropIntegrationTest, JsonRpcExampleServerRoundTripWorks) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  auto transport = std::make_unique<a2a::client::JsonRpcTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kJsonRpc,
                                     .url = "http://agent.local/rpc",
                                     .security_requirements = {},
                                     .security_schemes = {}},
      [&server](const a2a::client::HttpRequest& request) {
        const auto response = server.Handle({.method = request.method,
                                             .target = a2a::examples::UrlToTarget(request.url),
                                             .headers = request.headers,
                                             .body = request.body,
                                             .remote_address = {}});
        if (!response.ok()) {
          return a2a::core::Result<a2a::client::HttpClientResponse>(response.error());
        }
        return a2a::core::Result<a2a::client::HttpClientResponse>(a2a::client::HttpClientResponse{
            .status_code = response.value().status_code,
            .headers = response.value().headers,
            .body = response.value().body,
        });
      });

  a2a::client::A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id("interop-json-rpc-msg-1");
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->add_parts()->set_text("hello");
  const auto send = client.SendMessage(request);
  ASSERT_TRUE(send.ok()) << send.error().message();

  lf::a2a::v1::GetTaskRequest get;
  get.set_id(send.value().task().id());
  const auto loaded = client.GetTask(get);
  ASSERT_TRUE(loaded.ok()) << loaded.error().message();
  EXPECT_EQ(loaded.value().id(), send.value().task().id());
}

}  // namespace
