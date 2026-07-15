// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/error.h"

namespace {

constexpr int kHttpOk = 200;
constexpr std::chrono::milliseconds kWaitTimeout{2000};
constexpr std::chrono::milliseconds kCustomTimeout{750};
constexpr std::string_view kRestEndpoint = "https://agent.example.com/a2a";
constexpr std::string_view kJsonRpcEndpoint = "https://agent.example.com/rpc";
constexpr std::string_view kTaskId = "task-1";

class RecordingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    {
      std::lock_guard lock(mutex_);
      events_.push_back(response);
    }
    condition_.notify_all();
  }

  void OnError(const a2a::core::Error& error) override {
    {
      std::lock_guard lock(mutex_);
      errors_.push_back(error);
    }
    condition_.notify_all();
  }

  void OnCompleted() override {
    {
      std::lock_guard lock(mutex_);
      ++completion_count_;
    }
    condition_.notify_all();
  }

  [[nodiscard]] bool WaitForTerminal() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, kWaitTimeout, [this] { return completion_count_ > 0 || !errors_.empty(); });
  }

  [[nodiscard]] std::vector<lf::a2a::v1::StreamResponse> events() const {
    std::lock_guard lock(mutex_);
    return events_;
  }

  [[nodiscard]] std::vector<a2a::core::Error> errors() const {
    std::lock_guard lock(mutex_);
    return errors_;
  }

  [[nodiscard]] int completion_count() const {
    std::lock_guard lock(mutex_);
    return completion_count_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::vector<a2a::core::Error> errors_;
  int completion_count_ = 0;
};

a2a::client::ResolvedInterface MakeRestInterface() {
  return {.transport = a2a::client::PreferredTransport::kRest,
          .url = std::string(kRestEndpoint),
          .security_requirements = {},
          .security_schemes = {}};
}

a2a::client::ResolvedInterface MakeJsonRpcInterface() {
  return {.transport = a2a::client::PreferredTransport::kJsonRpc,
          .url = std::string(kJsonRpcEndpoint),
          .security_requirements = {},
          .security_schemes = {}};
}

lf::a2a::v1::SendMessageRequest MakeSendRequest() {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_task_id(std::string(kTaskId));
  return request;
}

a2a::core::Result<a2a::client::HttpClientResponse> UnusedRequester(const a2a::client::HttpRequest&) {
  return a2a::core::Error::Internal("unused unary requester");
}

TEST(HttpJsonStreamingGapTest, PropagatesOptionsAndAcceptsContentTypeParameters) {
  a2a::client::HttpRequest captured;
  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      MakeRestInterface(), UnusedRequester,
      [&captured](const a2a::client::HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                  const a2a::client::HttpStreamChunkHandler& on_chunk,
                  const a2a::client::StreamCancelled&) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        captured = request;
        a2a::client::HttpClientResponse response{
            .status_code = kHttpOk,
            .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream; charset=utf-8"}},
            .body = {}};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return metadata.error();
        }
        const auto chunk = on_chunk(
            R"(data: {"statusUpdate":{"taskId":"task-1","status":{"state":"TASK_STATE_WORKING"}}}

)");
        if (!chunk.ok()) {
          return chunk.error();
        }
        return response;
      });

  a2a::client::A2AClient client(std::move(transport));
  RecordingObserver observer;
  a2a::client::CallOptions options;
  options.timeout = kCustomTimeout;
  options.headers["X-Test-Header"] = "streaming";
  options.extensions = {"urn:example:streaming"};
  options.mtls = a2a::client::MtlsConfig{.client_certificate_pem = "cert",
                                         .client_private_key_pem = "key",
                                         .trusted_ca_pem = "ca",
                                         .server_name_override = "agent.example.com"};

  auto handle = client.SendStreamingMessage(MakeSendRequest(), observer, options);
  ASSERT_TRUE(handle.ok()) << handle.error().message();
  ASSERT_TRUE(observer.WaitForTerminal());

  EXPECT_EQ(captured.method, "POST");
  EXPECT_EQ(captured.url, std::string(kRestEndpoint) + "/message:stream");
  EXPECT_EQ(captured.timeout, kCustomTimeout);
  EXPECT_EQ(captured.headers.at("Accept"), "text/event-stream");
  EXPECT_EQ(captured.headers.at("X-Test-Header"), "streaming");
  EXPECT_TRUE(captured.mtls.has_value());
  EXPECT_EQ(observer.events().size(), 1U);
  EXPECT_TRUE(observer.errors().empty());
  EXPECT_EQ(observer.completion_count(), 1);
}

TEST(JsonRpcStreamingGapTest, SubscribeUsesExpectedMethodAndMissingResponseIdFailsOnce) {
  a2a::client::HttpRequest captured;
  auto transport = std::make_unique<a2a::client::JsonRpcTransport>(
      MakeJsonRpcInterface(), UnusedRequester,
      [&captured](const a2a::client::HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                  const a2a::client::HttpStreamChunkHandler& on_chunk,
                  const a2a::client::StreamCancelled&) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        captured = request;
        a2a::client::HttpClientResponse response{
            .status_code = kHttpOk,
            .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
            .body = {}};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return metadata.error();
        }
        const auto chunk = on_chunk(
            R"(data: {"jsonrpc":"2.0","result":{"statusUpdate":{"taskId":"task-1","status":{"state":"TASK_STATE_WORKING"}}}}

)");
        if (!chunk.ok()) {
          return chunk.error();
        }
        return response;
      },
      a2a::client::JsonRpcTransport::kDefaultTimeout, [] { return "stream-id"; });

  a2a::client::A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kTaskId));

  auto handle = client.SubscribeTask(request, observer);
  ASSERT_TRUE(handle.ok()) << handle.error().message();
  ASSERT_TRUE(observer.WaitForTerminal());

  EXPECT_EQ(captured.method, "POST");
  EXPECT_EQ(captured.url, kJsonRpcEndpoint);
  EXPECT_NE(captured.body.find("a2a.subscribeToTask"), std::string::npos);
  EXPECT_NE(captured.body.find("stream-id"), std::string::npos);
  EXPECT_TRUE(observer.events().empty());
  ASSERT_EQ(observer.errors().size(), 1U);
  EXPECT_EQ(observer.completion_count(), 0);
}

TEST(JsonRpcStreamingGapTest, DecodesStatusAndArtifactEventsFromOneChunk) {
  auto transport = std::make_unique<a2a::client::JsonRpcTransport>(
      MakeJsonRpcInterface(), UnusedRequester,
      [](const a2a::client::HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
         const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        a2a::client::HttpClientResponse response{
            .status_code = kHttpOk,
            .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
            .body = {}};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return metadata.error();
        }
        const auto chunks = on_chunk(
            R"(data: {"jsonrpc":"2.0","id":"stream-id","result":{"statusUpdate":{"taskId":"task-1","status":{"state":"TASK_STATE_WORKING"}}}}

data: {"jsonrpc":"2.0","id":"stream-id","result":{"artifactUpdate":{"taskId":"task-1","artifact":{"artifactId":"artifact-1"}}}}

)");
        if (!chunks.ok()) {
          return chunks.error();
        }
        return response;
      },
      a2a::client::JsonRpcTransport::kDefaultTimeout, [] { return "stream-id"; });

  a2a::client::A2AClient client(std::move(transport));
  RecordingObserver observer;
  auto handle = client.SendStreamingMessage(MakeSendRequest(), observer);
  ASSERT_TRUE(handle.ok()) << handle.error().message();
  ASSERT_TRUE(observer.WaitForTerminal());

  const auto events = observer.events();
  ASSERT_EQ(events.size(), 2U);
  EXPECT_TRUE(events[0].has_status_update());
  EXPECT_TRUE(events[1].has_artifact_update());
  EXPECT_TRUE(observer.errors().empty());
  EXPECT_EQ(observer.completion_count(), 1);
}

TEST(StreamHandleLifecycleGapTest, NoObserverCallbacksOccurAfterCancelReturns) {
  std::mutex mutex;
  std::condition_variable condition;
  bool started = false;
  bool release = false;
  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      MakeRestInterface(), UnusedRequester,
      [&mutex, &condition, &started, &release](
          const a2a::client::HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
          const a2a::client::HttpStreamChunkHandler& on_chunk,
          const a2a::client::StreamCancelled& is_cancelled) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        a2a::client::HttpClientResponse response{
            .status_code = kHttpOk,
            .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
            .body = {}};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return metadata.error();
        }
        {
          std::lock_guard lock(mutex);
          started = true;
        }
        condition.notify_all();
        {
          std::unique_lock lock(mutex);
          condition.wait_for(lock, kWaitTimeout, [&release, &is_cancelled] { return release || is_cancelled(); });
        }
        const auto late_chunk = on_chunk(
            R"(data: {"statusUpdate":{"taskId":"task-1","status":{"state":"TASK_STATE_WORKING"}}}

)");
        if (!late_chunk.ok()) {
          return late_chunk.error();
        }
        return response;
      });

  a2a::client::A2AClient client(std::move(transport));
  RecordingObserver observer;
  auto handle = client.SendStreamingMessage(MakeSendRequest(), observer);
  ASSERT_TRUE(handle.ok()) << handle.error().message();
  {
    std::unique_lock lock(mutex);
    ASSERT_TRUE(condition.wait_for(lock, kWaitTimeout, [&started] { return started; }));
  }

  handle.value()->Cancel();
  const auto events_after_cancel = observer.events().size();
  const auto errors_after_cancel = observer.errors().size();
  const auto completions_after_cancel = observer.completion_count();
  {
    std::lock_guard lock(mutex);
    release = true;
  }
  condition.notify_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  EXPECT_EQ(observer.events().size(), events_after_cancel);
  EXPECT_EQ(observer.errors().size(), errors_after_cancel);
  EXPECT_EQ(observer.completion_count(), completions_after_cancel);
}

}  // namespace
