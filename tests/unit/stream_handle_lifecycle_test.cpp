// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string_view>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"

namespace {

constexpr std::chrono::milliseconds kWaitTimeout{2000};
constexpr std::chrono::milliseconds kPromptCancelTimeout{500};
constexpr std::chrono::milliseconds kCancelPollInterval{10};
constexpr int kHttpOk = 200;
constexpr int kHttpServerError = 500;
constexpr char kContentTypeHeader[] = "content-type";
constexpr char kEventStreamContentType[] = "text/event-stream";
constexpr char kEndpointUrl[] = "http://127.0.0.1/a2a";

class CountingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    (void)response;
    std::lock_guard lock(mutex_);
    ++events_;
    cv_.notify_all();
  }

  void OnError(const a2a::core::Error& error) override {
    (void)error;
    std::lock_guard lock(mutex_);
    ++errors_;
    cv_.notify_all();
  }

  void OnCompleted() override {
    std::lock_guard lock(mutex_);
    ++completions_;
    cv_.notify_all();
  }

  [[nodiscard]] bool WaitForTerminal() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kWaitTimeout, [this] { return completions_ > 0 || errors_ > 0; });
  }

  [[nodiscard]] int completions() const {
    std::lock_guard lock(mutex_);
    return completions_;
  }

  [[nodiscard]] int errors() const {
    std::lock_guard lock(mutex_);
    return errors_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  int events_ = 0;
  int errors_ = 0;
  int completions_ = 0;
};

class BlockingStreamFixture final {
 public:
  [[nodiscard]] a2a::client::HttpStreamRequester MakeRequester() {
    return
        [this](const a2a::client::HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& on_metadata,
               const a2a::client::HttpStreamChunkHandler& on_chunk, const a2a::client::StreamCancelled& is_cancelled) {
          (void)request;
          (void)on_chunk;
          const a2a::client::HttpClientResponse response{
              .status_code = kHttpOk, .headers = {{kContentTypeHeader, kEventStreamContentType}}, .body = {}};
          const auto metadata = on_metadata(response);
          if (!metadata.ok()) {
            return a2a::core::Result<a2a::client::HttpClientResponse>(metadata.error());
          }
          {
            std::lock_guard lock(mutex_);
            started_ = true;
          }
          cv_.notify_all();
          while (!is_cancelled()) {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, kCancelPollInterval);
          }
          return a2a::core::Result<a2a::client::HttpClientResponse>(response);
        };
  }

  [[nodiscard]] bool WaitUntilStarted() {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, kWaitTimeout, [this] { return started_; });
  }

  void Wake() { cv_.notify_all(); }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool started_ = false;
};

[[nodiscard]] a2a::client::ResolvedInterface MakeResolvedRest() {
  return {.transport = a2a::client::PreferredTransport::kRest,
          .url = kEndpointUrl,
          .security_requirements = {},
          .security_schemes = {}};
}

[[nodiscard]] lf::a2a::v1::SendMessageRequest MakeRequest() {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_task_id("stream-handle-task");
  return request;
}

[[nodiscard]] std::unique_ptr<a2a::client::A2AClient> MakeClient(a2a::client::HttpStreamRequester requester) {
  auto transport = std::make_unique<a2a::client::HttpJsonTransport>(
      MakeResolvedRest(),
      [](const a2a::client::HttpRequest&) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        return a2a::core::Error::Internal("unary requester is unused");
      },
      std::move(requester));
  return std::make_unique<a2a::client::A2AClient>(std::move(transport));
}

TEST(StreamHandleLifecycleTest, IsActiveWhileWorkIsActiveAndCancelIsIdempotent) {
  BlockingStreamFixture fixture;
  auto client = MakeClient(fixture.MakeRequester());
  CountingObserver observer;

  auto handle = client->SendStreamingMessage(MakeRequest(), observer);
  ASSERT_TRUE(handle.ok()) << handle.error().message();
  ASSERT_TRUE(fixture.WaitUntilStarted());
  EXPECT_TRUE(handle.value()->IsActive());

  const auto before_cancel = std::chrono::steady_clock::now();
  handle.value()->Cancel();
  handle.value()->Cancel();
  const auto elapsed = std::chrono::steady_clock::now() - before_cancel;

  EXPECT_LT(elapsed, kPromptCancelTimeout);
  EXPECT_FALSE(handle.value()->IsActive());
  EXPECT_EQ(observer.completions(), 0);
  EXPECT_EQ(observer.errors(), 0);
}

TEST(StreamHandleLifecycleTest, DestroyingActiveHandleCancelsAndJoinsSafely) {
  BlockingStreamFixture fixture;
  auto client = MakeClient(fixture.MakeRequester());
  CountingObserver observer;

  {
    auto handle = client->SendStreamingMessage(MakeRequest(), observer);
    ASSERT_TRUE(handle.ok()) << handle.error().message();
    ASSERT_TRUE(fixture.WaitUntilStarted());
  }
  fixture.Wake();

  EXPECT_EQ(observer.completions(), 0);
  EXPECT_EQ(observer.errors(), 0);
}

TEST(StreamHandleLifecycleTest, CompletionIsReportedAtMostOnce) {
  auto client = MakeClient(
      [](const a2a::client::HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& on_metadata,
         const a2a::client::HttpStreamChunkHandler& on_chunk, const a2a::client::StreamCancelled& is_cancelled) {
        (void)request;
        (void)on_chunk;
        (void)is_cancelled;
        const a2a::client::HttpClientResponse response{
            .status_code = kHttpOk, .headers = {{kContentTypeHeader, kEventStreamContentType}}, .body = {}};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return a2a::core::Result<a2a::client::HttpClientResponse>(metadata.error());
        }
        return a2a::core::Result<a2a::client::HttpClientResponse>(response);
      });
  CountingObserver observer;

  auto handle = client->SendStreamingMessage(MakeRequest(), observer);
  ASSERT_TRUE(handle.ok()) << handle.error().message();
  ASSERT_TRUE(observer.WaitForTerminal());

  EXPECT_EQ(observer.completions(), 1);
  EXPECT_EQ(observer.errors(), 0);
  EXPECT_FALSE(handle.value()->IsActive());
}

TEST(StreamHandleLifecycleTest, ErrorAndCompletionAreMutuallyExclusive) {
  auto client = MakeClient(
      [](const a2a::client::HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& on_metadata,
         const a2a::client::HttpStreamChunkHandler& on_chunk, const a2a::client::StreamCancelled& is_cancelled) {
        (void)request;
        (void)on_chunk;
        (void)is_cancelled;
        const a2a::client::HttpClientResponse response{
            .status_code = kHttpServerError, .headers = {{kContentTypeHeader, kEventStreamContentType}}, .body = {}};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return a2a::core::Result<a2a::client::HttpClientResponse>(metadata.error());
        }
        return a2a::core::Result<a2a::client::HttpClientResponse>(response);
      });
  CountingObserver observer;

  auto handle = client->SendStreamingMessage(MakeRequest(), observer);
  ASSERT_TRUE(handle.ok()) << handle.error().message();
  ASSERT_TRUE(observer.WaitForTerminal());

  EXPECT_EQ(observer.errors(), 1);
  EXPECT_EQ(observer.completions(), 0);
  EXPECT_FALSE(handle.value()->IsActive());
}

}  // namespace
