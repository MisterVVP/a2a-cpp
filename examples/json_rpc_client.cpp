#include <iostream>
#include <memory>

#include "a2a/client/client.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/server.h"
#include "example_support.h"

int main() {
  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  auto transport = std::make_unique<a2a::client::JsonRpcTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kJsonRpc,
                                     .url = "http://agent.local/rpc",
                                     .security_requirements = {},
                                     .security_schemes = {}},
      [&server](const a2a::client::HttpRequest& request)
          -> a2a::core::Result<a2a::client::HttpClientResponse> {
        const auto response = server.Handle({.method = request.method,
                                             .target = a2a::examples::UrlToTarget(request.url),
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
  request.mutable_message()->set_task_id("json-rpc-example-task");

  const auto send = client.SendMessage(request);
  if (!send.ok()) {
    std::cerr << "send failed: " << send.error().message() << '\n';
    return 1;
  }

  std::cout << "json-rpc created task: " << send.value().task().id() << '\n';
  return 0;
}
