#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/error.h"

namespace {

using a2a::client::A2AClient;
using a2a::client::CallOptions;
using a2a::client::HttpClientResponse;
using a2a::client::HttpJsonTransport;
using a2a::client::HttpRequest;
using a2a::client::ResolvedInterface;
using a2a::client::StreamObserver;

ResolvedInterface MakeResolvedRest() {
  ResolvedInterface resolved;
  resolved.transport = a2a::client::PreferredTransport::kRest;
  resolved.url = "https://agent.example.com/a2a";
  return resolved;
}

class RecordingObserver final : public StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    std::lock_guard<std::mutex> lock(mu_);
    events.push_back(response);
  }

  void OnError(const a2a::core::Error& error) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      errors.push_back(error);
    }
    cv_.notify_all();
  }

  void OnCompleted() override {
    completed = true;
    cv_.notify_all();
  }

  bool WaitForCompletion(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(wait_mu_);
    return cv_.wait_for(lock, timeout, [this] { return completed || !errors.empty(); });
  }

  std::vector<lf::a2a::v1::StreamResponse> events;
  std::vector<a2a::core::Error> errors;
  bool completed = false;

 private:
  std::mutex mu_;
  std::mutex wait_mu_;
  std::condition_variable cv_;
};

TEST(HttpJsonStreamingIntegrationTest, SendStreamingMessageParsesFragmentedEventsInOrder) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Internal("unused");
      },
      [](const HttpRequest& request, const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        EXPECT_EQ(request.method, "POST");
        EXPECT_EQ(request.url, "https://agent.example.com/a2a/messages:stream");
        EXPECT_EQ(request.headers.at("Accept"), "text/event-stream");

        const std::vector<std::string> chunks = {
            "event: message\ndata: {\"task\":{\"id\":\"t-1\"}}\n\n",
            "event: message\ndata: {\"statusUpdate\":{\"taskId\":\"t-1\",\"final\":false,",
            "\"status\":{\"state\":\"TASK_STATE_WORKING\"}}}\n\n",
            "data: "
            "{\"artifactUpdate\":{\"taskId\":\"t-1\",\"artifact\":{\"artifactId\":\"a-1\"}}}\n\n"};

        for (const auto& chunk : chunks) {
          const auto status = on_chunk(chunk);
          if (!status.ok()) {
            return status.error();
          }
        }

        return HttpClientResponse{
            .status_code = 200, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role("user");

  auto stream = client.SendStreamingMessage(request, observer, CallOptions{});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.errors.empty());
  ASSERT_EQ(observer.events.size(), 3U);
  EXPECT_EQ(observer.events[0].task().id(), "t-1");
  EXPECT_EQ(observer.events[1].status_update().status().state(), lf::a2a::v1::TASK_STATE_WORKING);
  EXPECT_EQ(observer.events[2].artifact_update().artifact().artifact_id(), "a-1");
  EXPECT_TRUE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, MalformedFrameTriggersObserverError) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Internal("unused");
      },
      [](const HttpRequest&, const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        const auto chunk_status = on_chunk("broken-field\n\n");
        if (!chunk_status.ok()) {
          return chunk_status.error();
        }
        return HttpClientResponse{
            .status_code = 200, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role("user");

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_FALSE(observer.errors.empty());
  EXPECT_FALSE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, CancelDuringActiveStreamStopsWithoutCompletion) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Internal("unused");
      },
      [](const HttpRequest&, const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled& is_cancelled)
          -> a2a::core::Result<HttpClientResponse> {
        for (int i = 0; i < 100; ++i) {
          if (is_cancelled()) {
            break;
          }
          const auto status = on_chunk("data: {\"task\":{\"id\":\"t-1\"}}\n\n");
          if (!status.ok()) {
            return status.error();
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return HttpClientResponse{
            .status_code = 200, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role("user");

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_TRUE(stream.value()->IsActive());
  stream.value()->Cancel();

  EXPECT_FALSE(stream.value()->IsActive());
  EXPECT_FALSE(observer.events.empty());
  EXPECT_TRUE(observer.errors.empty());
  EXPECT_FALSE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, RemoteCloseWithoutTerminalEventCompletes) {
  auto transport = std::make_unique<HttpJsonTransport>(
      MakeResolvedRest(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Internal("unused");
      },
      [](const HttpRequest&, const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        const auto status = on_chunk("data: {\"task\":{\"id\":\"t-1\"}}\n\n");
        if (!status.ok()) {
          return status.error();
        }
        return HttpClientResponse{
            .status_code = 200, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  auto stream = client.SubscribeTask(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.errors.empty());
  EXPECT_TRUE(observer.completed);
  ASSERT_EQ(observer.events.size(), 1U);
  EXPECT_EQ(observer.events[0].task().id(), "t-1");
}

}  // namespace
