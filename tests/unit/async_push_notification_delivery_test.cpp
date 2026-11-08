// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <barrier>
#include <stdexcept>
#include <thread>

#include "a2a/core/protojson.h"
#include "support/async_push_test_support.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::testing;

class AsyncPushDeliveryTest : public ::testing::Test {
 protected:
  void TearDown() override { client.Shutdown(); }
  PushProbe probe;
  FakePushRequester fake;
  HttpAsyncPushNotificationDeliveryClient client{PushOptions(), fake.Requester()};
};

TEST_F(AsyncPushDeliveryTest, OwnsPayloadAndPreservesWireFormat) {
  auto request = PushRequest();
  const auto json = core::MessageToJson(request.payload);
  ASSERT_TRUE(json.ok());
  auto operation = client.DeliverAsync(request, probe.Callback());
  ASSERT_TRUE(operation.ok());
  request = {};
  const auto sent = fake.Request();
  EXPECT_EQ(sent.url, kPushUrl);
  EXPECT_EQ(sent.method, core::http::kMethodPost);
  EXPECT_EQ(sent.body, json.value());
  ASSERT_EQ(sent.headers.size(), kPushAttempts);
  EXPECT_EQ(sent.headers.front().name, core::http::kContentTypeHeaderName);
  EXPECT_EQ(sent.headers.front().value, core::http::kContentTypeApplicationJson);
  EXPECT_EQ(sent.headers.back().name, core::http::kAuthorizationHeaderName);
  EXPECT_EQ(sent.headers.back().value, kPushAuthorization);
  fake.Complete();
  const auto result = probe.Get();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().http_status, kPushSuccess);
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
}

TEST_F(AsyncPushDeliveryTest, MapsNonSuccessAndNetworkErrors) {
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  fake.Complete(kPushError);
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kRemoteProtocol);
  EXPECT_EQ(result.error().http_status(), kPushError);
  operation.value().Wait();
}
TEST_F(AsyncPushDeliveryTest, ForwardsRequesterError) {
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  fake.Token().Complete(core::Error::Network(kPushFailure));
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().message(), kPushFailure);
  EXPECT_EQ(result.error().code(), core::ErrorCode::kNetwork);
  operation.value().Wait();
}
TEST_F(AsyncPushDeliveryTest, ValidatesBeforeSending) {
  auto operation = client.DeliverAsync(PushRequest(kPushUnsupportedUrl), probe.Callback());
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kValidation);
  EXPECT_EQ(fake.Count(), 0U);
  operation.value().Wait();
}
TEST_F(AsyncPushDeliveryTest, CancellationWakesRequesterAndCompletesOnce) {
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  operation.value().Cancel();
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::AsyncHandle::kCancelled);
  fake.Token().handle().Wait();
  fake.Complete();
  operation.value().Wait();
  EXPECT_FALSE(fake.Token().IsActive());
  EXPECT_EQ(probe.calls.load(), 1U);
}
TEST_F(AsyncPushDeliveryTest, TimeoutCancelsRequester) {
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback(), {.timeout = kPushShort, .deadline = {}});
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::AsyncHandle::kDeadlineExceeded);
  fake.Token().handle().Wait();
  operation.value().Wait();
}
TEST_F(AsyncPushDeliveryTest, ExpiredDeadlineDoesNotSend) {
  auto operation =
      client.DeliverAsync(PushRequest(), probe.Callback(),
                          {.timeout = kPushLong, .deadline = std::chrono::steady_clock::time_point::min()});
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::AsyncHandle::kDeadlineExceeded);
  operation.value().Wait();
}
TEST_F(AsyncPushDeliveryTest, ZeroTimeoutExpiresWithoutSending) {
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback(),
                                       {.timeout = std::chrono::milliseconds::zero(), .deadline = {}});
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::AsyncHandle::kDeadlineExceeded);
  EXPECT_EQ(fake.Count(), 0U);
  operation.value().Wait();
}
TEST_F(AsyncPushDeliveryTest, OmitsAuthorizationWithoutScheme) {
  auto request = PushRequest();
  request.config.mutable_authentication()->clear_scheme();
  auto operation = client.DeliverAsync(request, probe.Callback());
  ASSERT_TRUE(operation.ok());
  EXPECT_EQ(fake.Request().headers.size(), 1U);
  fake.Complete();
  EXPECT_TRUE(probe.Get().ok());
  operation.value().Wait();
}
TEST_F(AsyncPushDeliveryTest, ShutdownDrainsAndClosesAdmission) {
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  client.Shutdown();
  EXPECT_EQ(probe.calls.load(), 1U);
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), core::AsyncHandle::kCancelled);
  EXPECT_FALSE(client.DeliverAsync(PushRequest(), probe.Callback()).ok());
  EXPECT_EQ(probe.calls.load(), 1U);
}
TEST_F(AsyncPushDeliveryTest, MissingCallbackIsRejectedWithoutSending) {
  EXPECT_FALSE(client.DeliverAsync(PushRequest(), {}).ok());
}
TEST_F(AsyncPushDeliveryTest, CancelCompleteRaceIsOnceOnly) {
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  std::barrier race(kPushAttempts);
  std::thread completing([&] {
    race.arrive_and_wait();
    fake.Complete();
  });
  race.arrive_and_wait();
  operation.value().Cancel();
  completing.join();
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
  const auto result = probe.Get();
  EXPECT_TRUE(result.ok() || result.error().protocol_code() == core::AsyncHandle::kCancelled);
}

TEST(AsyncPushContractTest, FallbackPreservesPayloadAndCancellation) {
  PushProbe probe;
  FakePushRequester fake;
  auto options = PushOptions();
  options.http_version = std::string(core::http::kHttpVersion20);
  options.fallback_http_version = std::string(core::http::kHttpVersion11);
  HttpAsyncPushNotificationDeliveryClient client(options, fake.Requester());
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  fake.Complete(kPushError);
  ASSERT_TRUE(fake.WaitFor(kPushAttempts));
  EXPECT_EQ(fake.Request(kPushSecond).body, fake.Request().body);
  EXPECT_EQ(fake.Request(kPushSecond).http_version, options.fallback_http_version);
  EXPECT_LE(fake.Request(kPushSecond).timeout, fake.Request().timeout);
  operation.value().Cancel();
  fake.Token(kPushSecond).handle().Wait();
  EXPECT_FALSE(fake.Token(kPushSecond).IsActive());
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
}
TEST(AsyncPushContractTest, MissingRequesterCompletesWithValidationError) {
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions(), {});
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kValidation);
  operation.value().Wait();
}
TEST(AsyncPushContractTest, InlinePrimaryCompletionCannotReplaceFallbackCancellation) {
  PushProbe probe;
  FakePushRequester fake;
  auto options = PushOptions();
  options.http_version = std::string(core::http::kHttpVersion20);
  options.fallback_http_version = std::string(core::http::kHttpVersion11);
  std::atomic<std::size_t> calls{0};
  HttpAsyncPushNotificationDeliveryClient client(options, [&](http::Request request, auto completion) {
    if (++calls != 1U) {
      return fake.Requester()(std::move(request), std::move(completion));
    }
    auto first = fake.Requester()(std::move(request), [](auto result) { (void)result; });
    completion(http::Response{.status_code = kPushError, .headers = {}, .body = {}});
    // A broken requester must not start a duplicate fallback or complete twice.
    completion(http::Response{.status_code = kPushError, .headers = {}, .body = {}});
    return first;
  });
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  ASSERT_TRUE(fake.WaitFor(kPushAttempts));
  EXPECT_EQ(calls.load(), kPushAttempts);
  operation.value().Cancel();
  fake.Token(kPushSecond).handle().Wait();
  EXPECT_FALSE(fake.Token(kPushSecond).IsActive());
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
}
TEST(AsyncPushContractTest, FallbackCanSucceedWithoutResettingDeadline) {
  PushProbe probe;
  FakePushRequester fake;
  auto options = PushOptions();
  options.http_version = std::string(core::http::kHttpVersion20);
  options.fallback_http_version = std::string(core::http::kHttpVersion11);
  HttpAsyncPushNotificationDeliveryClient client(options, fake.Requester());
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  fake.Complete(kPushError);
  ASSERT_TRUE(fake.WaitFor(kPushAttempts));
  fake.Complete(kPushSuccess, kPushSecond);
  EXPECT_TRUE(probe.Get().ok());
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
}
TEST(AsyncPushContractTest, RequesterRejectionBecomesAsyncCompletion) {
  PushProbe probe;
  const auto caller = std::this_thread::get_id();
  HttpAsyncPushNotificationDeliveryClient client(PushOptions(),
                                                 [](auto request, auto completion) -> core::Result<core::AsyncHandle> {
                                                   (void)request;
                                                   (void)completion;
                                                   return core::Error::Network(kPushFailure);
                                                 });
  auto operation = client.DeliverAsync(PushRequest(), [&](auto result) {
    EXPECT_NE(std::this_thread::get_id(), caller);
    probe.Callback()(std::move(result));
  });
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().message(), kPushFailure);
  operation.value().Wait();
}
TEST(AsyncPushContractTest, CallbackCanDestroyClient) {
  PushProbe probe;
  FakePushRequester fake;
  auto client = std::make_unique<HttpAsyncPushNotificationDeliveryClient>(PushOptions(), fake.Requester());
  auto operation = client->DeliverAsync(PushRequest(), [&](auto result) {
    client.reset();
    probe.Callback()(std::move(result));
  });
  ASSERT_TRUE(operation.ok());
  fake.Complete();
  EXPECT_TRUE(probe.Get().ok());
  operation.value().Wait();
}
TEST(AsyncPushContractTest, InvalidFallbackVersionCompletesBeforeSending) {
  PushProbe probe;
  FakePushRequester fake;
  auto options = PushOptions();
  options.fallback_http_version = kPushUnknownVersion;
  HttpAsyncPushNotificationDeliveryClient client(options, fake.Requester());
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kValidation);
  EXPECT_EQ(fake.Count(), 0U);
  operation.value().Wait();
}
#if !defined(A2A_HAS_LIBCURL)
TEST(AsyncPushContractTest, DefaultRequesterReportsDisabledLibcurlSupport) {
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions());
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kValidation);
  operation.value().Wait();
}
#endif
TEST(AsyncPushContractTest, RequesterExceptionBecomesInternalCompletion) {
  PushProbe probe;
  HttpAsyncPushNotificationDeliveryClient client(PushOptions(),
                                                 [](auto request, auto completion) -> core::Result<core::AsyncHandle> {
                                                   (void)request;
                                                   (void)completion;
                                                   throw std::runtime_error(kPushFailure);
                                                 });
  auto operation = client.DeliverAsync(PushRequest(), probe.Callback());
  ASSERT_TRUE(operation.ok());
  const auto result = probe.Get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kInternal);
  operation.value().Wait();
  EXPECT_EQ(probe.calls.load(), 1U);
}
}  // namespace
