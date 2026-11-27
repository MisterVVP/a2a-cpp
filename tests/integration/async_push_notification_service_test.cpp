// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_push_notification_service.h"

#include <gtest/gtest.h>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>

#include "support/async_push_test_support.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
using namespace a2a::server::testing;
constexpr auto kSecondConfig = "second-config";

class FixedPushClient final : public AsyncPushNotificationDeliveryClient {
 public:
  explicit FixedPushClient(PushDeliveryResult response) : response_(std::move(response)) {}
  core::Result<core::AsyncHandle> DeliverAsync(const PushDeliveryRequest& request,
                                               core::Completion<PushDeliveryResult> completion,
                                               const AsyncPushDeliveryOptions& options) override {
    (void)request;
    (void)options;
    auto started = scope_.Start<PushDeliveryResult>(std::move(completion), kPushLong);
    if (!started.ok()) {
      return started.error();
    }
    started.value().Complete(response_);
    return started.value().handle();
  }
  void Shutdown() override { scope_.Shutdown(); }

 private:
  core::AsyncScope scope_;
  PushDeliveryResult response_;
};

class CancelBeforeWaitClient final : public AsyncPushNotificationDeliveryClient {
 public:
  CancelBeforeWaitClient(std::shared_ptr<AsyncPushNotificationDeliveryClient> client,
                         boost::asio::cancellation_signal& cancellation)
      : client_(std::move(client)), cancellation_(cancellation) {}
  core::Result<core::AsyncHandle> DeliverAsync(const PushDeliveryRequest& request,
                                               core::Completion<PushDeliveryResult> completion,
                                               const AsyncPushDeliveryOptions& options) override {
    auto started = client_->DeliverAsync(request, std::move(completion), options);
    cancellation_.emit(boost::asio::cancellation_type::all);
    return started;
  }
  void Shutdown() override { client_->Shutdown(); }

 private:
  std::shared_ptr<AsyncPushNotificationDeliveryClient> client_;
  boost::asio::cancellation_signal& cancellation_;
};

class AsyncPushServiceTest : public ::testing::Test {
 protected:
  AsyncPushServiceTest() {
    task.set_id(kPushTask);
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
    EXPECT_TRUE(store.CreateOrUpdate(PushRequest().config).ok());
  }
  void TearDown() override {
    client->Shutdown();
    loop.restart();
    loop.run_for(kPushWait);
  }
  std::future<core::Result<void>> Start(AsyncPushDeliveryOptions options = {}) {
    auto result = boost::asio::co_spawn(
        loop, AsyncPushNotificationService::NotifyTaskUpdated(store, client, task, execution.Context(), options),
        boost::asio::use_future);
    loop.poll();
    return result;
  }
  template <typename T>
  core::Result<T> Finish(std::future<core::Result<T>>& future) {
    loop.restart();
    loop.run_for(kPushWait);
    const auto ready = future.wait_for(std::chrono::seconds::zero());
    EXPECT_EQ(ready, std::future_status::ready);
    if (ready != std::future_status::ready) {
      return core::Error::Internal(kPushFailure);
    }
    return future.get();
  }
  boost::asio::io_context loop;
  FakePushRequester fake;
  std::shared_ptr<HttpAsyncPushNotificationDeliveryClient> client =
      std::make_shared<HttpAsyncPushNotificationDeliveryClient>(PushOptions(), fake.Requester());
  InMemoryPushNotificationStore store;
  RequestExecution execution;
  lf::a2a::v1::Task task;
};
TEST_F(AsyncPushServiceTest, AwaitingDeliveryLeavesSingleAsioWorkerAvailable) {
  auto result = Start();
  ASSERT_TRUE(fake.WaitFor(1));
  bool unrelated = false;
  boost::asio::post(loop, [&] { unrelated = true; });
  loop.poll();
  EXPECT_TRUE(unrelated);
  EXPECT_EQ(result.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
  fake.Complete();
  EXPECT_TRUE(Finish(result).ok());
}
TEST_F(AsyncPushServiceTest, ContextCancellationStopsDelivery) {
  auto result = Start();
  ASSERT_TRUE(fake.WaitFor(1));
  EXPECT_TRUE(execution.Cancel());
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().protocol_code(), core::AsyncHandle::kCancelled);
  fake.Token().handle().Wait();
  EXPECT_FALSE(fake.Token().IsActive());
}
TEST_F(AsyncPushServiceTest, DeadlineStopsDelivery) {
  auto result = Start({.timeout = kPushShort, .deadline = {}});
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().protocol_code(), core::AsyncHandle::kDeadlineExceeded);
}
TEST_F(AsyncPushServiceTest, AsioCancellationCancelsOutboundHandle) {
  boost::asio::cancellation_signal cancelled;
  auto result = boost::asio::co_spawn(
      loop, AsyncPushNotificationService::NotifyTaskUpdated(store, client, task, execution.Context()),
      boost::asio::bind_cancellation_slot(cancelled.slot(), boost::asio::use_future));
  loop.poll();
  ASSERT_TRUE(fake.WaitFor(1));
  cancelled.emit(boost::asio::cancellation_type::all);
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().protocol_code(), core::AsyncHandle::kCancelled);
  fake.Token().handle().Wait();
  EXPECT_FALSE(fake.Token().IsActive());
}
TEST_F(AsyncPushServiceTest, CancellationBeforeWaitDoesNotLeakOutboundDelivery) {
  boost::asio::cancellation_signal cancellation;
  auto cancelling = std::make_shared<CancelBeforeWaitClient>(client, cancellation);
  auto future = boost::asio::co_spawn(
      loop, AsyncPushNotificationService::NotifyTaskUpdated(store, cancelling, task, execution.Context()),
      boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
  loop.poll();
  ASSERT_TRUE(fake.WaitFor(1));
  core::Result<void> delivered;
  EXPECT_NO_THROW(delivered = Finish(future));
  EXPECT_FALSE(fake.Token().IsActive());
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().protocol_code(), core::AsyncHandle::kCancelled);
}

TEST_F(AsyncPushServiceTest, FirstErrorStopsConfigFanout) {
  auto config = PushRequest().config;
  config.set_id(kSecondConfig);
  ASSERT_TRUE(store.CreateOrUpdate(config).ok());
  auto result = Start();
  ASSERT_TRUE(fake.WaitFor(1));
  fake.Complete(kPushError);
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().http_status(), kPushError);
}
TEST_F(AsyncPushServiceTest, NoConfigsCompletesWithoutHttp) {
  ASSERT_TRUE(store.Delete(kPushTask, kPushConfig).ok());
  auto result = Start();
  EXPECT_TRUE(Finish(result).ok());
}
TEST_F(AsyncPushServiceTest, AlreadyCancelledDoesNotSend) {
  EXPECT_TRUE(execution.Cancel());
  auto result = Start();
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().protocol_code(), core::AsyncHandle::kCancelled);
}
TEST_F(AsyncPushServiceTest, ClientShutdownResumesSuspendedCoroutine) {
  auto result = Start();
  ASSERT_TRUE(fake.WaitFor(1));
  client->Shutdown();
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().protocol_code(), core::AsyncHandle::kCancelled);
}
TEST_F(AsyncPushServiceTest, RequestContextDeadlineLimitsDelivery) {
  RequestExecution source({}, RequestExecution::Clock::now() + kPushShort);
  auto result = boost::asio::co_spawn(
      loop, AsyncPushNotificationService::NotifyTaskUpdated(store, client, task, source.Context()),
      boost::asio::use_future);
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().protocol_code(), core::AsyncHandle::kDeadlineExceeded);
}
TEST_F(AsyncPushServiceTest, RejectsCustomDeliveryErrorMessage) {
  auto custom =
      std::make_shared<FixedPushClient>(PushDeliveryResult{.http_status = kPushSuccess, .error_message = kPushFailure});
  auto result = boost::asio::co_spawn(
      loop, AsyncPushNotificationService::NotifyTaskUpdated(store, custom, task, execution.Context()),
      boost::asio::use_future);
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().message(), kPushFailure);
}
TEST_F(AsyncPushServiceTest, RejectsCustomNonSuccessStatus) {
  auto custom = std::make_shared<FixedPushClient>(PushDeliveryResult{.http_status = kPushError, .error_message = {}});
  auto result = boost::asio::co_spawn(
      loop, AsyncPushNotificationService::NotifyTaskUpdated(store, custom, task, execution.Context()),
      boost::asio::use_future);
  const auto delivered = Finish(result);
  ASSERT_FALSE(delivered.ok());
  EXPECT_EQ(delivered.error().http_status(), kPushError);
}
}  // namespace
