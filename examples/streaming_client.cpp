#include <iostream>
#include <memory>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/protojson.h"

namespace {

class PrintingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    if (response.has_status_update()) {
      std::cout << "status event: " << response.status_update().status().state() << '\n';
    }
  }

  void OnError(const a2a::core::Error& error) override { std::cerr << error.message() << '\n'; }

  void OnCompleted() override { std::cout << "stream completed\n"; }
};

}  // namespace

int main() {
  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kRest,
                                     .url = "http://agent.local/a2a",
                                     .security_requirements = {},
                                     .security_schemes = {}},
      [](const a2a::client::HttpRequest& request)
          -> a2a::core::Result<a2a::client::HttpClientResponse> {
        (void)request;
        return a2a::core::Error::Validation("non-streaming not used in this example");
      },
      [](const a2a::client::HttpRequest& request,
         const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled& is_cancelled)
          -> a2a::core::Result<a2a::client::HttpClientResponse> {
        (void)request;
        if (is_cancelled()) {
          return a2a::client::HttpClientResponse{.status_code = 499, .headers = {}, .body = {}};
        }

        lf::a2a::v1::StreamResponse event;
        event.mutable_status_update()->set_task_id("stream-example-task");
        event.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
        const auto json = a2a::core::MessageToJson(event);
        if (!json.ok()) {
          return json.error();
        }

        const auto status = on_chunk(std::string("data: ") + json.value() + "\n\n");
        if (!status.ok()) {
          return status.error();
        }

        return a2a::client::HttpClientResponse{
            .status_code = 200,
            .headers = {{"content-type", "text/event-stream"}},
            .body = {},
        };
      });

  a2a::client::A2AClient client(std::move(transport));
  PrintingObserver observer;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role("user");
  request.mutable_message()->set_task_id("stream-example-task");

  const auto handle = client.SendStreamingMessage(request, observer);
  if (!handle.ok()) {
    std::cerr << "stream failed: " << handle.error().message() << '\n';
    return 1;
  }

  return 0;
}
