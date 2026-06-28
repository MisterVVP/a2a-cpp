// SPDX-License-Identifier: Apache-2.0

#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/protojson.h"

namespace {

constexpr char kTaskId[] = "stream-client-task";
constexpr char kContentTypeHeader[] = "content-type";
constexpr char kEventStreamContentType[] = "text/event-stream";

class PrintingObserver final : public a2a::client::StreamObserver {
 public:
  void WaitForCompletion() {
    std::unique_lock<std::mutex> lock(mutex_);
    completion_cv_.wait(lock, [this]() { return completed_; });
  }

  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    if (response.has_status_update()) {
      std::cout << "streaming_client event task id: " << response.status_update().task_id() << '\n';
      std::cout << "streaming_client event state: " << response.status_update().status().state() << '\n';
    }
  }

  void OnError(const a2a::core::Error& error) override {
    std::cerr << "streaming_client error: " << error.message() << '\n';
    MarkCompleted();
  }

  void OnCompleted() override {
    std::cout << "streaming_client completed\n";
    MarkCompleted();
  }

 private:
  void MarkCompleted() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      completed_ = true;
    }
    completion_cv_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable completion_cv_;
  bool completed_ = false;
};

}  // namespace

int main() {
  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      a2a::client::ResolvedInterface{.transport = a2a::client::PreferredTransport::kRest,
                                     .url = "http://agent.local/a2a",
                                     .security_requirements = {},
                                     .security_schemes = {}},
      [](const a2a::client::HttpRequest& request) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        (void)request;
        return a2a::core::Error::Validation("non-streaming calls are not used");
      },
      [](const a2a::client::HttpRequest& request, const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled& is_cancelled) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        (void)request;
        if (is_cancelled()) {
          return a2a::client::HttpClientResponse{.status_code = 499, .headers = {}, .body = {}};
        }
        lf::a2a::v1::StreamResponse event;
        event.mutable_status_update()->set_task_id(kTaskId);
        event.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
        const auto json = a2a::core::MessageToJson(event);
        if (!json.ok()) {
          return json.error();
        }
        std::string frame;
        frame.reserve(json.value().size() + 8);
        frame.append("data: ");
        frame.append(json.value());
        frame.append("\n\n");
        const auto status = on_chunk(frame);
        if (!status.ok()) {
          return status.error();
        }
        return a2a::client::HttpClientResponse{
            .status_code = 200, .headers = {{kContentTypeHeader, kEventStreamContentType}}, .body = {}};
      });

  a2a::client::A2AClient client(std::move(transport));
  PrintingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_task_id(kTaskId);

  const auto handle = client.SendStreamingMessage(request, observer);
  if (!handle.ok()) {
    std::cerr << "streaming_client failed: " << handle.error().message() << '\n';
    return 1;
  }
  observer.WaitForCompletion();
  return 0;
}
