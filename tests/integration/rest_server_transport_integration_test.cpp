#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "../support/rest_server_test_utils.h"
#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"

namespace {

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

class RestIntegrationHarness final {
 public:
  RestIntegrationHarness()
      : executor_(&store_),
        dispatcher_(&executor_),
        server_(&dispatcher_,
                a2a::tests::support::BuildRestAgentCard("Integration REST Agent",
                                                        "http://agent.local/a2a"),
                {.rest_api_base_path = "/a2a"}) {}

  a2a::client::DiscoveryClient CreateDiscoveryClient() {
    return a2a::client::DiscoveryClient(
        [this](std::string_view url) { return FetchAgentCard(url); });
  }

  std::unique_ptr<a2a::client::HttpJsonTransport> CreateTransport(
      const a2a::client::ResolvedInterface& resolved) {
    return std::make_unique<a2a::client::HttpJsonTransport>(
        resolved, [this](const a2a::client::HttpRequest& request) { return SendHttp(request); });
  }

  a2a::core::Result<a2a::server::HttpServerResponse> Handle(std::string method, std::string target,
                                                            a2a::client::HeaderMap headers = {}) {
    return server_.Handle(a2a::tests::support::MakeHttpRequest(std::move(method), std::move(target),
                                                               std::move(headers)));
  }

 private:
  a2a::core::Result<a2a::client::HttpResponse> FetchAgentCard(std::string_view url) {
    const auto response =
        server_.Handle(a2a::tests::support::MakeHttpRequest("GET", UrlToTarget(url), {}, {}, {}));
    if (!response.ok()) {
      return response.error();
    }
    return a2a::client::HttpResponse{
        .status_code = response.value().status_code,
        .body = response.value().body,
    };
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
    return a2a::client::HttpClientResponse{
        .status_code = response.value().status_code,
        .headers = response.value().headers,
        .body = response.value().body,
    };
  }

  a2a::server::InMemoryTaskStore store_;
  a2a::tests::support::StoreExecutor executor_;
  a2a::server::Dispatcher dispatcher_;
  a2a::server::RestServerTransport server_;
};

a2a::core::Result<a2a::client::ResolvedInterface> DiscoverRestInterface(
    RestIntegrationHarness* harness) {
  if (harness == nullptr) {
    return a2a::core::Error::Internal("Harness is required");
  }

  auto discovery = harness->CreateDiscoveryClient();
  const auto card = discovery.Fetch("http://agent.local");
  if (!card.ok()) {
    return card.error();
  }

  return a2a::client::AgentCardResolver::SelectPreferredInterface(
      card.value(), a2a::client::PreferredTransport::kRest);
}

TEST(RestServerTransportIntegrationTest, DiscoveryAndA2AClientRoundTripWorks) {
  RestIntegrationHarness harness;

  const auto resolved = DiscoverRestInterface(&harness);
  ASSERT_TRUE(resolved.ok());

  auto transport = harness.CreateTransport(resolved.value());
  a2a::client::A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role("user");
  send_request.mutable_message()->set_task_id("rest-integration-1");

  const auto send_response = client.SendMessage(send_request);
  ASSERT_TRUE(send_response.ok());
  EXPECT_EQ(send_response.value().task().id(), "rest-integration-1");

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("rest-integration-1");
  const auto get_response = client.GetTask(get_request);
  ASSERT_TRUE(get_response.ok());
  EXPECT_EQ(get_response.value().status().state(), lf::a2a::v1::TASK_STATE_WORKING);

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id("rest-integration-1");
  const auto cancel_response = client.CancelTask(cancel_request);
  ASSERT_TRUE(cancel_response.ok());
  EXPECT_EQ(cancel_response.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);

  const auto list_response =
      harness.Handle("GET", "/a2a/tasks?pageSize=10", {{"A2A-Version", "1.0"}});
  ASSERT_TRUE(list_response.ok());
  EXPECT_EQ(list_response.value().status_code, 200);
  EXPECT_NE(list_response.value().body.find("rest-integration-1"), std::string::npos);
}

TEST(RestServerTransportIntegrationTest, MissingVersionHeaderIsRejected) {
  RestIntegrationHarness harness;

  const auto response = harness.Handle("GET", "/a2a/tasks");

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 426);
}

}  // namespace
