#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "../support/rest_server_test_utils.h"
#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/protojson.h"
#include "a2a/server/json_rpc_server_transport.h"

namespace {

constexpr int kHttpOk = 200;

std::string UrlToTarget(std::string_view url) {
  const std::size_t scheme = url.find("://");
  if (scheme == std::string_view::npos) {
    return std::string(url);
  }

  const std::size_t path_start = url.find('/', scheme + 3);
  if (path_start == std::string_view::npos) {
    return "/";
  }
  return std::string(url.substr(path_start));
}

class JsonRpcIntegrationHarness final {
 public:
  JsonRpcIntegrationHarness()
      : executor_(&store_),
        dispatcher_(&executor_),
        card_(a2a::tests::support::BuildJsonRpcAgentCard("Integration JSON-RPC Agent",
                                                         "http://agent.local/rpc")),
        server_(&dispatcher_, {.rpc_path = "/rpc"}) {
    card_.set_protocol_version("1.0");
  }

  a2a::client::DiscoveryClient CreateDiscoveryClient() {
    return a2a::client::DiscoveryClient(
        [this](std::string_view url) { return FetchAgentCard(url); });
  }

  std::unique_ptr<a2a::client::JsonRpcTransport> CreateTransport(
      const a2a::client::ResolvedInterface& resolved) {
    return std::make_unique<a2a::client::JsonRpcTransport>(
        resolved, [this](const a2a::client::HttpRequest& request) { return SendHttp(request); });
  }

 private:
  a2a::core::Result<a2a::client::HttpResponse> FetchAgentCard(std::string_view url) {
    if (UrlToTarget(url) != "/.well-known/agent-card.json") {
      return a2a::core::Error::Validation("Agent card endpoint mismatch");
    }
    const auto body = a2a::core::MessageToJson(card_);
    if (!body.ok()) {
      return body.error();
    }
    return a2a::client::HttpResponse{.status_code = kHttpOk, .body = body.value()};
  }

  a2a::core::Result<a2a::client::HttpClientResponse> SendHttp(
      const a2a::client::HttpRequest& request) {
    const auto response = server_.Handle({.method = request.method,
                                          .target = UrlToTarget(request.url),
                                          .headers = request.headers,
                                          .body = request.body,
                                          .remote_address = {}});
    if (!response.ok()) {
      return response.error();
    }
    return a2a::client::HttpClientResponse{.status_code = response.value().status_code,
                                           .headers = response.value().headers,
                                           .body = response.value().body};
  }

  a2a::server::InMemoryTaskStore store_;
  a2a::tests::support::StoreExecutor executor_;
  a2a::server::Dispatcher dispatcher_;
  lf::a2a::v1::AgentCard card_;
  a2a::server::JsonRpcServerTransport server_;
};

TEST(JsonRpcServerTransportIntegrationTest, DiscoveryAndClientRoundTripWorks) {
  JsonRpcIntegrationHarness harness;

  auto discovery = harness.CreateDiscoveryClient();
  const auto card = discovery.Fetch("http://agent.local");
  ASSERT_TRUE(card.ok());

  const auto resolved = a2a::client::AgentCardResolver::SelectPreferredInterface(
      card.value(), a2a::client::PreferredTransport::kJsonRpc);
  ASSERT_TRUE(resolved.ok());

  auto transport = harness.CreateTransport(resolved.value());
  a2a::client::A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role("user");
  send_request.mutable_message()->set_task_id("jsonrpc-integration-1");

  const auto send_response = client.SendMessage(send_request);
  ASSERT_TRUE(send_response.ok());
  EXPECT_EQ(send_response.value().task().id(), "jsonrpc-integration-1");

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("jsonrpc-integration-1");
  const auto get_response = client.GetTask(get_request);
  ASSERT_TRUE(get_response.ok());
  EXPECT_EQ(get_response.value().status().state(), lf::a2a::v1::TASK_STATE_WORKING);

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id("jsonrpc-integration-1");
  const auto cancel_response = client.CancelTask(cancel_request);
  ASSERT_TRUE(cancel_response.ok());
  EXPECT_EQ(cancel_response.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

}  // namespace
