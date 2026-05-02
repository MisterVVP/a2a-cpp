#include <iostream>
#include <memory>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "example_support.h"

int main() {
  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher,
      a2a::examples::BuildRestAgentCard("PushConfig Example Agent", "http://agent.local/a2a"),
      {.rest_api_base_path = "/a2a"});

  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kRest,
                                     .url = "http://agent.local/a2a"},
      [&server](const a2a::client::HttpRequest& request)
          -> a2a::core::Result<a2a::client::HttpClientResponse> {
        const auto response = server.Handle({.method = request.method,
                                             .target = a2a::examples::UrlToTarget(request.url),
                                             .headers = request.headers,
                                             .body = request.body});
        if (!response.ok()) {
          return response.error();
        }
        return a2a::client::HttpClientResponse{.status_code = response.value().status_code,
                                               .headers = response.value().headers,
                                               .body = response.value().body};
      });

  a2a::client::A2AClient client(std::move(transport));

  lf::a2a::v1::TaskPushNotificationConfig cfg;
  cfg.set_id("cfg-1");
  cfg.set_task_id("task-1");
  cfg.set_endpoint("https://callback.example/notify");
  const auto set_result = client.SetTaskPushNotificationConfig(cfg);
  if (!set_result.ok()) {
    std::cerr << "set failed: " << set_result.error().message() << '\n';
    return 1;
  }

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  list_request.set_task_id("task-1");
  const auto list_result = client.ListTaskPushNotificationConfigs(list_request);
  if (!list_result.ok()) {
    std::cerr << "list failed: " << list_result.error().message() << '\n';
    return 1;
  }

  std::cout << "push configs: " << list_result.value().configs_size() << '\n';
  return 0;
}
