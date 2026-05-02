#include <iostream>
#include <memory>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "a2a/server/server.h"
#include "example_support.h"

class LoggingInterceptor final : public a2a::client::ClientInterceptor {
 public:
  void BeforeCall(const a2a::client::ClientCallContext& context) override {
    std::cout << "before " << context.operation << '\n';
  }

  void AfterCall(const a2a::client::ClientCallContext& context,
                 const a2a::client::ClientCallResult& result) override {
    std::cout << "after " << context.operation << " ok=" << result.ok << '\n';
  }
};

int main() {
  a2a::examples::ExampleExecutor executor;
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::RestServerTransport server(
      &dispatcher,
      a2a::examples::BuildRestAgentCard("Interceptor Example Agent", "http://agent.local/a2a"),
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
  client.AddInterceptor(std::make_shared<LoggingInterceptor>());

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("missing-task-id");
  const auto get = client.GetTask(request);
  if (get.ok()) {
    std::cerr << "unexpected success" << '\n';
    return 1;
  }

  std::cout << "expected error: " << get.error().message() << '\n';
  return 0;
}
