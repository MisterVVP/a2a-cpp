// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <iostream>
#include <memory>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "example_support.h"

int main() {
  constexpr const char* kListTasksSeedMessageId = "list-tasks-example-seed-1";
  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher, a2a::examples::BuildRestAgentCard("ListTasks Example Agent", "http://agent.local/a2a"),
      {.rest_api_base_path = "/a2a"});

  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kRest,
                                     .url = "http://agent.local/a2a",
                                     .security_requirements = {},
                                     .security_schemes = {}},
      [&server](const a2a::client::HttpRequest& request) -> a2a::core::Result<a2a::client::HttpClientResponse> {
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
  lf::a2a::v1::SendMessageRequest seed;
  seed.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  seed.mutable_message()->set_message_id(kListTasksSeedMessageId);
  *seed.mutable_message()->add_parts()->mutable_text() = "hello from ListTasks example";
  const auto send = client.SendMessage(seed);
  if (!send.ok()) {
    std::cerr << "seed send failed: " << send.error().message() << '\n';
    return 1;
  }

  a2a::client::ListTasksRequest list_request;
  list_request.page_size = 10;
  const auto list = client.ListTasks(list_request);
  if (!list.ok()) {
    std::cerr << "list failed: " << list.error().message() << '\n';
    return 1;
  }

  std::cout << "listed tasks: " << list.value().tasks.size() << '\n';
  return 0;
}
