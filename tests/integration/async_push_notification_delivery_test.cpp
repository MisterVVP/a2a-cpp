// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#if defined(A2A_HAS_LIBCURL)
#include <array>
#include <atomic>
#include <future>
#include <thread>
#include <vector>

#include "support/async_push_test_support.h"
#include "support/curl_reactor_test_support.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::testing;
using http::testing::CurlLoopbackServer;
constexpr auto kEmptyResponse = "";
constexpr auto kSuccessHeaders = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
constexpr auto kErrorHeaders = "HTTP/1.1 503 Unavailable\r\nContent-Length: 1\r\nConnection: close\r\n\r\n";
constexpr auto kErrorBody = "x";
constexpr std::size_t kThreadBound = 9;
constexpr std::array<std::size_t, 3> kConcurrency{1, 16, 64};

TEST(AsyncPushNetworkTest, SuccessMatchesSynchronousDelivery) {
  CurlLoopbackServer server(kPushAttempts, kEmptyResponse, kSuccessHeaders);
  HttpPushNotificationDeliveryClient sync(PushOptions());
  const auto baseline = sync.Deliver(PushRequest(server.url()));
  ASSERT_TRUE(baseline.ok());
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto operation = client.DeliverAsync(PushRequest(server.url()), probe.Callback());
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().http_status, baseline.value().http_status);
  EXPECT_EQ(result.value().error_message, baseline.value().error_message);
  operation.value().Wait();
}
TEST(AsyncPushNetworkTest, NonSuccessMatchesSyncErrorMapping) {
  CurlLoopbackServer server(kPushAttempts, kErrorBody, kErrorHeaders);
  HttpPushNotificationDeliveryClient sync(PushOptions());
  const auto baseline = sync.Deliver(PushRequest(server.url()));
  ASSERT_FALSE(baseline.ok());
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto operation = client.DeliverAsync(PushRequest(server.url()), probe.Callback());
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().http_status(), baseline.error().http_status());
  EXPECT_EQ(result.error().code(), baseline.error().code());
  EXPECT_EQ(result.error().message(), baseline.error().message());
  operation.value().Wait();
}
TEST(AsyncPushNetworkTest, ConnectionFailureCompletesOnce) {
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kNetwork);
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
}
TEST(AsyncPushNetworkTest, CancelWakesOpenDelivery) {
  CurlLoopbackServer server(1, kEmptyResponse);
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto operation = client.DeliverAsync(PushRequest(server.url()), probe.Callback());
  ASSERT_TRUE(operation.ok());
  ASSERT_TRUE(server.WaitForStreams(1));
  operation.value().Cancel();
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::AsyncHandle::kCancelled);
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
}
TEST(AsyncPushNetworkTest, DelayedResponseExpires) {
  CurlLoopbackServer server(1, kEmptyResponse);
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto operation =
      client.DeliverAsync(PushRequest(server.url()), probe.Callback(), {.timeout = kPushShort, .deadline = {}});
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::AsyncHandle::kDeadlineExceeded);
  operation.value().Wait();
}
TEST(AsyncPushNetworkTest, ShutdownCancelsOpenDeliveriesAndDrainsCallbacks) {
  CurlLoopbackServer server(1, kEmptyResponse);
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto operation = client.DeliverAsync(PushRequest(server.url()), probe.Callback());
  ASSERT_TRUE(operation.ok());
  ASSERT_TRUE(server.WaitForStreams(1));
  client.Shutdown();
  EXPECT_EQ(probe.calls.load(), 1U);
  EXPECT_FALSE(client.DeliverAsync(PushRequest(server.url()), probe.Callback()).ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::AsyncHandle::kCancelled);
}
TEST(AsyncPushNetworkTest, SlowCallbackDoesNotStallUnrelatedDelivery) {
  CurlLoopbackServer server(kPushAttempts, kEmptyResponse, kSuccessHeaders);
  std::promise<void> release;
  auto gate = release.get_future().share();
  std::promise<void> entered;
  auto ready = entered.get_future();
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto slow = client.DeliverAsync(PushRequest(server.url()), [gate, &entered](auto result) {
    EXPECT_TRUE(result.ok());
    entered.set_value();
    (void)gate.wait_for(kPushWait);
  });
  ASSERT_TRUE(slow.ok());
  EXPECT_EQ(ready.wait_for(kPushWait), std::future_status::ready);
  auto fast = client.DeliverAsync(PushRequest(server.url()), probe.Callback());
  ASSERT_TRUE(fast.ok());
  EXPECT_TRUE(probe.Get().ok());
  release.set_value();
  fast.value().Wait();
  slow.value().Wait();
}
TEST(AsyncPushNetworkTest, CancelCompleteRaceRemainsOnceOnly) {
  CurlLoopbackServer server(1, kEmptyResponse);
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto operation = client.DeliverAsync(PushRequest(server.url()), probe.Callback());
  ASSERT_TRUE(operation.ok());
  ASSERT_TRUE(server.WaitForStreams(1));
  std::thread finish([&] { (void)server.Finish(kPushFirst, kEmptyResponse); });
  operation.value().Cancel();
  finish.join();
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
  const auto result = probe.Get();
  EXPECT_TRUE(result.ok() || result.error().protocol_code() == core::AsyncHandle::kCancelled);
}

void CountCompleted(core::Result<PushDeliveryResult> result, std::atomic<std::size_t>& completed) {
  EXPECT_TRUE(result.ok());
  ++completed;
}
void FinishDeliveries(CurlLoopbackServer& server, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    EXPECT_TRUE(server.Finish(index, kEmptyResponse));
  }
}
void CheckConcurrency(std::size_t count) {
  CurlLoopbackServer server(count, kEmptyResponse);
  std::atomic<std::size_t> completed{0};
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  const auto threads = http::testing::ReactorTestThreadCount();
  std::vector<core::AsyncHandle> handles;
  handles.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    auto operation = client.DeliverAsync(PushRequest(server.url()),
                                         [&](auto result) { CountCompleted(std::move(result), completed); });
    ASSERT_TRUE(operation.ok());
    handles.push_back(std::move(operation.value()));
  }
  ASSERT_TRUE(server.WaitForStreams(count));
  EXPECT_LE(http::testing::ReactorTestThreadCount(), threads + kThreadBound);
  FinishDeliveries(server, count);
  for (const auto& handle : handles) {
    handle.Wait();
  }
  EXPECT_EQ(completed.load(), count);
}
TEST(AsyncPushNetworkTest, IncreasingConcurrentDeliveriesUseBoundedThreads) {
  for (const auto count : kConcurrency) {
    CheckConcurrency(count);
  }
}
}  // namespace
#endif
