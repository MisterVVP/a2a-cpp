#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "../support/rest_server_test_utils.h"
#include "a2a/client/auth.h"
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

class AuthCapturingExecutor final : public a2a::server::AgentExecutor {
 public:
  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    observed_bearer_token = context.auth_metadata["bearer_token"];
    observed_api_key = context.auth_metadata["api_key"];

    lf::a2a::v1::SendMessageResponse response;
    response.mutable_task()->set_id(request.message().task_id());
    response.mutable_task()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    return response;
  }

  a2a::core::Result<std::unique_ptr<a2a::server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Validation("streaming not implemented");
  }

  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(
      const a2a::server::ListTasksRequest& request, a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::server::ListTasksResponse{};
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }

  std::string observed_bearer_token;
  std::string observed_api_key;
};

TEST(RestServerTransportIntegrationTest, AuthHeadersPropagateToServerContext) {
  AuthCapturingExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher,
      a2a::tests::support::BuildRestAgentCard("Integration REST Auth Agent",
                                              "http://agent.local/a2a"),
      {.rest_api_base_path = "/a2a"});

  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kRest,
                                     .url = "http://agent.local/a2a",
                                     .security_requirements = {},
                                     .security_schemes = {}},
      [&server](const a2a::client::HttpRequest& request)
          -> a2a::core::Result<a2a::client::HttpClientResponse> {
        const auto response = server.Handle({.method = request.method,
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
      });

  a2a::client::A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role("user");
  request.mutable_message()->set_task_id("auth-integration-1");

  a2a::client::CallOptions options;
  options.credential_provider =
      std::make_shared<a2a::client::BearerTokenCredentialProvider>("integration-token");
  options.headers["X-API-Key"] = "integration-api-key";

  const auto response = client.SendMessage(request, options);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(executor.observed_bearer_token, "integration-token");
  EXPECT_EQ(executor.observed_api_key, "integration-api-key");
}

}  // namespace
