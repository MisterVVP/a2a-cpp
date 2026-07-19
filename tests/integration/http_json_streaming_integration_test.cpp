// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/error.h"
#include "a2a/core/http_constants.h"

namespace {

using a2a::client::A2AClient;
using a2a::client::CallOptions;
using a2a::client::HttpClientResponse;
using a2a::client::HttpJsonTransport;
using a2a::client::HttpRequest;
using a2a::client::ResolvedInterface;
using a2a::client::StreamObserver;

constexpr std::string_view kStreamingSendUrl = "https://agent.example.com/a2a/message:stream";
constexpr int kHttpOk = 200;
constexpr int kHttpBadGateway = 502;
constexpr int kStreamLoopMaxIterations = 100;
constexpr int kCancelDelayMs = 30;
constexpr std::chrono::milliseconds kRequestCaptureTimeout{2000};
constexpr std::string_view kAcceptHeaderName = "Accept";

ResolvedInterface MakeResolvedRest();

a2a::core::Result<HttpClientResponse> UnusedHttpRequester(const HttpRequest& request) {
  (void)request;
  return a2a::core::Error::Internal("unused");
}

a2a::core::Result<HttpClientResponse> EmitSseChunks(const std::vector<std::string>& chunks,
                                                    const a2a::client::HttpStreamMetadataHandler& on_metadata,
                                                    const a2a::client::HttpStreamChunkHandler& on_chunk) {
  HttpClientResponse response{
      .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}}, .body = ""};
  const auto metadata = on_metadata(response);
  if (!metadata.ok()) {
    return metadata.error();
  }
  for (const auto& chunk : chunks) {
    const auto status = on_chunk(chunk);
    if (!status.ok()) {
      return status.error();
    }
  }
  return response;
}

a2a::core::Result<HttpClientResponse> EmitMetadataOnly(HttpClientResponse response,
                                                       const a2a::client::HttpStreamMetadataHandler& on_metadata) {
  const auto metadata = on_metadata(response);
  if (!metadata.ok()) {
    return metadata.error();
  }
  return response;
}

std::unique_ptr<HttpJsonTransport> MakeStreamingTransport(
    const std::function<a2a::core::Result<HttpClientResponse>(
        const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
        const a2a::client::StreamCancelled&)>& stream_requester) {
  return std::make_unique<HttpJsonTransport>(MakeResolvedRest(), UnusedHttpRequester, stream_requester);
}

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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HttpJsonStreamingIntegrationTest, SendStreamingMessageParsesFragmentedEventsInOrder) {
  const std::vector<std::string> chunks = {
      "event: message\ndata: {\"task\":{\"id\":\"t-1\"}}\n\n",
      "event: message\ndata: {\"statusUpdate\":{\"taskId\":\"t-1\",",
      "\"status\":{\"state\":\"TASK_STATE_WORKING\"}}}\n\n",
      "data: {\"artifactUpdate\":{\"taskId\":\"t-1\",\"artifact\":{\"artifactId\":\"a-1\"}}}\n\n"};

  auto transport = MakeStreamingTransport(
      [chunks](const HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& on_metadata,
               const a2a::client::HttpStreamChunkHandler& on_chunk,
               const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        if (request.method != "POST") {
          return a2a::core::Error::Internal("unexpected method");
        }
        if (request.url != kStreamingSendUrl) {
          return a2a::core::Error::Internal("unexpected url");
        }
        if (request.headers.at("Accept") != "text/event-stream") {
          return a2a::core::Error::Internal("unexpected accept header");
        }
        return EmitSseChunks(chunks, on_metadata, on_chunk);
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

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

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HttpJsonStreamingIntegrationTest, MalformedFrameTriggersObserverError) {
  auto transport =
      MakeStreamingTransport([](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                                const a2a::client::HttpStreamChunkHandler& on_chunk,
                                const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return EmitSseChunks({"broken-field\n\n"}, on_metadata, on_chunk);
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_FALSE(observer.errors.empty());
  EXPECT_FALSE(observer.completed);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HttpJsonStreamingIntegrationTest, CancelDuringActiveStreamStopsWithoutCompletion) {
  auto transport = MakeStreamingTransport(
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
         const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled& is_cancelled) -> a2a::core::Result<HttpClientResponse> {
        HttpClientResponse response{.status_code = kHttpOk,
                                    .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                    .body = ""};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return metadata.error();
        }
        for (int i = 0; i < kStreamLoopMaxIterations; ++i) {
          if (is_cancelled()) {
            break;
          }
          const auto status = on_chunk("data: {\"task\":{\"id\":\"t-1\"}}\n\n");
          if (!status.ok()) {
            return status.error();
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return response;
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  std::this_thread::sleep_for(std::chrono::milliseconds(kCancelDelayMs));
  EXPECT_TRUE(stream.value()->IsActive());
  stream.value()->Cancel();

  EXPECT_FALSE(stream.value()->IsActive());
  EXPECT_FALSE(observer.events.empty());
  EXPECT_TRUE(observer.errors.empty());
  EXPECT_FALSE(observer.completed);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HttpJsonStreamingIntegrationTest, RemoteCloseWithoutTerminalEventCompletes) {
  auto transport =
      MakeStreamingTransport([](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                                const a2a::client::HttpStreamChunkHandler& on_chunk,
                                const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return EmitSseChunks({"data: {\"task\":{\"id\":\"t-1\"}}\n\n"}, on_metadata, on_chunk);
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

TEST(HttpJsonStreamingIntegrationTest, SubscribeTaskBuildsBodylessGetRequest) {
  std::promise<HttpRequest> request_promise;
  auto request_future = request_promise.get_future();
  auto transport = MakeStreamingTransport(
      [&request_promise](const HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                         const a2a::client::HttpStreamChunkHandler&,
                         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        request_promise.set_value(request);
        return EmitMetadataOnly(
            HttpClientResponse{.status_code = kHttpOk,
                               .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                               .body = ""},
            on_metadata);
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  auto stream = client.SubscribeTask(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_EQ(request_future.wait_for(kRequestCaptureTimeout), std::future_status::ready);
  const HttpRequest captured_request = request_future.get();
  ASSERT_TRUE(observer.WaitForCompletion(kRequestCaptureTimeout));
  stream.value()->Cancel();

  EXPECT_EQ(captured_request.method, a2a::core::http::kMethodGet);
  EXPECT_TRUE(captured_request.body.empty());
  EXPECT_FALSE(captured_request.headers.contains(std::string(a2a::core::http::kContentTypeHeaderName)));
  const auto accept = captured_request.headers.find(std::string(kAcceptHeaderName));
  ASSERT_NE(accept, captured_request.headers.end());
  EXPECT_EQ(accept->second, a2a::core::http::kContentTypeTextEventStream);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HttpJsonStreamingIntegrationTest, RemoteErrorEventMapsToObserverProtocolError) {
  auto transport =
      MakeStreamingTransport([](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                                const a2a::client::HttpStreamChunkHandler& on_chunk,
                                const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return EmitSseChunks({"event: error\ndata: {\"code\":\"TASK_FAILED\",\"message\":\"boom\"}\n\n"}, on_metadata,
                             on_chunk);
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  ASSERT_FALSE(observer.errors.empty());
  EXPECT_EQ(observer.errors.front().code(), a2a::core::ErrorCode::kRemoteProtocol);
  ASSERT_TRUE(observer.errors.front().protocol_code().has_value());
  EXPECT_EQ(observer.errors.front().protocol_code().value_or(""), "TASK_FAILED");
  EXPECT_FALSE(observer.completed);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
TEST(HttpJsonStreamingIntegrationTest, NonSuccessHttpStatusMapsToObserverError) {
  auto transport =
      MakeStreamingTransport([](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                                const a2a::client::HttpStreamChunkHandler&,
                                const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return EmitMetadataOnly(
            HttpClientResponse{.status_code = kHttpBadGateway,
                               .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                               .body = R"({"code":"UPSTREAM_FAILURE"})"},
            on_metadata);
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  ASSERT_FALSE(observer.errors.empty());
  EXPECT_EQ(observer.errors.front().code(), a2a::core::ErrorCode::kRemoteProtocol);
  ASSERT_TRUE(observer.errors.front().http_status().has_value());
  EXPECT_EQ(observer.errors.front().http_status().value_or(0), kHttpBadGateway);
  EXPECT_FALSE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, RejectsContentTypePrefixLookalike) {
  auto transport =
      MakeStreamingTransport([](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                                const a2a::client::HttpStreamChunkHandler&,
                                const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return EmitMetadataOnly(
            HttpClientResponse{.status_code = kHttpOk,
                               .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-streaming"}},
                               .body = ""},
            on_metadata);
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.events.empty());
  ASSERT_FALSE(observer.errors.empty());
  EXPECT_EQ(observer.errors.front().code(), a2a::core::ErrorCode::kRemoteProtocol);
  EXPECT_FALSE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, EmptyResponseWithoutMetadataCallbackStillCompletes) {
  auto transport = MakeStreamingTransport(
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.events.empty());
  EXPECT_TRUE(observer.errors.empty());
  EXPECT_TRUE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, ReturnedUnsupportedVersionIsValidatedForEmptyStream) {
  auto transport = MakeStreamingTransport(
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "2.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), a2a::core::ErrorCode::kUnsupportedVersion);
  EXPECT_FALSE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, ReturnedMissingContentTypeIsValidatedForEmptyStream) {
  auto transport = MakeStreamingTransport(
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForCompletion(std::chrono::milliseconds(2000)));
  stream.value()->Cancel();

  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), a2a::core::ErrorCode::kRemoteProtocol);
  EXPECT_FALSE(observer.completed);
}

TEST(HttpJsonStreamingIntegrationTest, SubscribeTaskWithoutIdReturnsValidationError) {
  auto transport = MakeStreamingTransport(
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Internal("unexpected stream requester call");
      });

  A2AClient client(std::move(transport));
  RecordingObserver observer;
  lf::a2a::v1::GetTaskRequest request;

  auto stream = client.SubscribeTask(request, observer);
  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(HttpJsonStreamingIntegrationTest, MissingStreamRequesterReturnsInternalError) {
  auto transport = std::make_unique<HttpJsonTransport>(MakeResolvedRest(), UnusedHttpRequester);
  A2AClient client(std::move(transport));
  RecordingObserver observer;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);

  const auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_FALSE(stream.ok());
  EXPECT_EQ(stream.error().code(), a2a::core::ErrorCode::kInternal);
}

}  // namespace
