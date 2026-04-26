#include <iostream>

#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "example_support.h"

int main() {
  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher,
      a2a::examples::BuildRestAgentCard("Minimal Server Example", "http://agent.local/a2a"),
      {.rest_api_base_path = "/a2a"});

  const auto card = server.Handle({.method = "GET",
                                   .target = "/.well-known/agent-card.json",
                                   .headers = {},
                                   .body = {},
                                   .remote_address = {}});
  if (!card.ok()) {
    std::cerr << "server failed: " << card.error().message() << '\n';
    return 1;
  }

  std::cout << "agent-card status: " << card.value().status_code << '\n';
  return 0;
}
