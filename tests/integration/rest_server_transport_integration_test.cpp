#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/error.h"
#include "a2a/core/protojson.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"

namespace {

class StoreExecutor final : public a2a::server::AgentExecutor {
 public:
  explicit StoreExecutor(a2a::server::TaskStore* store) : store_(store) {}

  a2a::core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request,
      a2a::server::RequestContext& context) override {
    (void)context;
    if (request.message().task_id().empty()) {
      return a2a::core::Error::Validation("message.task_id is required");
    }

    lf::a2a::v1::Task task;
    task.set_id(request.message().task_id());
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    const auto saved = store_->CreateOrUpdate(task);
    if (!saved.ok()) {
      return saved.error();
    }

    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = task;
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
    return store_->Get(request.id());
  }

  a2a::core::Result<a2a::server::ListTasksResponse> ListTasks(
      const a2a::server::ListTasksRequest& request, a2a::server::RequestContext& context) override {
    (void)context;
    return store_->List(request);
  }

  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    (void)context;
    return store_->Cancel(request.id());
  }

 private:
  a2a::server::TaskStore* store_;
};

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

lf::a2a::v1::AgentCard BuildCard() {
  lf::a2a::v1::AgentCard card;
  card.set_name("Integration REST Agent");
  auto* iface = card.add_supported_interfaces();
  iface->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_REST);
  iface->set_url("http://agent.local/a2a");
  return card;
}

TEST(RestServerTransportIntegrationTest, DiscoveryAndA2AClientRoundTripWorks) {
  a2a::server::InMemoryTaskStore store;
  StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  a2a::client::DiscoveryClient discovery([&server](std::string_view url) {
    const auto response =
        server.Handle({.method = "GET",
                       .target = UrlToTarget(url),
                       .headers = {},
                       .body = {},
                       .remote_address = {}});
    if (!response.ok()) {
      return a2a::core::Result<a2a::client::HttpResponse>(response.error());
    }
    return a2a::core::Result<a2a::client::HttpResponse>(a2a::client::HttpResponse{
        .status_code = response.value().status_code,
        .body = response.value().body,
    });
  });

  const auto card = discovery.Fetch("http://agent.local");
  ASSERT_TRUE(card.ok());

  const auto resolved =
      a2a::client::AgentCardResolver::SelectPreferredInterface(card.value(), a2a::client::PreferredTransport::kRest);
  ASSERT_TRUE(resolved.ok());

  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      resolved.value(), [&server](const a2a::client::HttpRequest& request) {
        const auto response = server.Handle({.method = request.method,
                                             .target = UrlToTarget(request.url),
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

  const auto list_response = server.Handle(
      {.method = "GET",
       .target = "/a2a/tasks?pageSize=10",
       .headers = {{"A2A-Version", "1.0"}},
       .body = {},
       .remote_address = {}});
  ASSERT_TRUE(list_response.ok());
  EXPECT_EQ(list_response.value().status_code, 200);
  EXPECT_NE(list_response.value().body.find("rest-integration-1"), std::string::npos);
}

TEST(RestServerTransportIntegrationTest, MissingVersionHeaderIsRejected) {
  a2a::server::InMemoryTaskStore store;
  StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(&dispatcher, BuildCard(), {.rest_api_base_path = "/a2a"});

  const auto response = server.Handle(
      {.method = "GET", .target = "/a2a/tasks", .headers = {}, .body = {}, .remote_address = {}});

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 426);
}

}  // namespace
