#include <iostream>

#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "example_support.h"

int main() {
  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher,
      a2a::examples::BuildRestAgentCard("Auth Policy Example Agent", "http://agent.local/a2a"),
      {.rest_api_base_path = "/a2a"});

  const auto unauthorized =
      server.Handle({.method = "GET", .target = "/a2a/tasks/task-1", .headers = {}, .body = {}});
  if (!unauthorized.ok()) {
    std::cerr << "request failed: " << unauthorized.error().message() << '\n';
    return 1;
  }
  std::cout << "missing auth status: " << unauthorized.value().status_code << '\n';

  const auto authorized = server.Handle({.method = "GET",
                                         .target = "/a2a/tasks/task-1",
                                         .headers = {{"authorization", "Bearer token"}},
                                         .body = {}});
  if (!authorized.ok()) {
    std::cerr << "request failed: " << authorized.error().message() << '\n';
    return 1;
  }
  std::cout << "auth metadata extracted, status: " << authorized.value().status_code << '\n';
  return 0;
}
