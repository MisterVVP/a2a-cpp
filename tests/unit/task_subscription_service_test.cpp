#include "a2a/server/task_subscription_service.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

static_assert(!std::is_copy_constructible_v<a2a::server::TaskSubscriptionService>);
static_assert(!std::is_copy_assignable_v<a2a::server::TaskSubscriptionService>);
static_assert(!std::is_move_constructible_v<a2a::server::TaskSubscriptionService>);
static_assert(!std::is_move_assignable_v<a2a::server::TaskSubscriptionService>);

constexpr std::string_view kTaskId = "subscription-task";
constexpr std::string_view kContextId = "subscription-context";
constexpr std::string_view kTransientArtifactId = "transient-artifact";
constexpr std::string_view kTransientHistoryMessageId = "transient-history-message";
constexpr std::string_view kFirstRaceContextId = "first-race-context";
constexpr std::string_view kSecondRaceContextId = "second-race-context";
constexpr std::chrono::milliseconds kSubscriptionWaitTimeout{1};
constexpr std::chrono::milliseconds kImmediateTimeout{0};
constexpr std::size_t kConcurrentCancelThreadCount = 8;
constexpr std::size_t kConcurrentPublisherCount = 8;
constexpr std::size_t kUpdatesPerPublisher = 16;
constexpr std::size_t kOrderingSubscriberCount = 8;
constexpr std::ptrdiff_t kConcurrentParticipantCount = 3;
constexpr std::size_t kMaximumRacingUpdateCount = 1;
constexpr std::size_t kExpectedConcurrentSignalEventCount = 3;

lf::a2a::v1::Task MakeTask(lf::a2a::v1::TaskState state) {
  lf::a2a::v1::Task task;
  task.set_id(std::string(kTaskId));
  task.set_context_id(std::string(kContextId));
  task.mutable_status()->set_state(state);
  return task;
}

lf::a2a::v1::StreamResponse NextRequired(a2a::server::ServerStreamSession* session) {
  auto next = session->Next();
  EXPECT_TRUE(next.ok());
  const auto& maybe_event = next.value();
  EXPECT_TRUE(maybe_event.has_value());
  if (!maybe_event.has_value()) {
    return {};
  }
  return *maybe_event;
}

void ExpectClosed(a2a::server::ServerStreamSession* session) {
  auto next = session->Next();
  EXPECT_TRUE(next.ok());
  EXPECT_FALSE(next.value().has_value());
}

std::vector<std::string> DrainStatusContextIds(a2a::server::ServerStreamSession* session) {
  std::vector<std::string> context_ids;
  while (true) {
    const auto next = session->Next();
    EXPECT_TRUE(next.ok());
    if (!next.ok()) {
      return context_ids;
    }
    const auto& maybe_event = next.value();
    if (!maybe_event.has_value()) {
      return context_ids;
    }
    const auto event = maybe_event.value_or(lf::a2a::v1::StreamResponse{});
    if (event.has_status_update()) {
      context_ids.push_back(event.status_update().context_id());
    }
  }
}

void RunConcurrently(const std::function<void()>& first, const std::function<void()>& second) {
  std::barrier start_gate(kConcurrentParticipantCount);
  std::thread first_thread([&] {
    start_gate.arrive_and_wait();
    first();
  });
  std::thread second_thread([&] {
    start_gate.arrive_and_wait();
    second();
  });
  start_gate.arrive_and_wait();
  first_thread.join();
  second_thread.join();
}

std::size_t DrainStatusUpdateCount(a2a::server::ServerStreamSession* session) {
  std::size_t count = 0;
  while (true) {
    const auto next = session->Next();
    EXPECT_TRUE(next.ok());
    if (!next.ok() || !next.value().has_value()) {
      return count;
    }
    count += next.value()->has_status_update() ? 1U : 0U;
  }
}

TEST(TaskSubscriptionServiceTest, FirstEventIsCurrentTask) {
  a2a::server::TaskSubscriptionService service;
  auto task = MakeTask(lf::a2a::v1::TASK_STATE_WORKING);
  task.add_artifacts()->set_artifact_id(std::string(kTransientArtifactId));
  task.add_history()->set_message_id(std::string(kTransientHistoryMessageId));
  auto subscription = service.Subscribe(task);
  ASSERT_TRUE(subscription.ok());

  const auto first = NextRequired(subscription.value().get());
  ASSERT_TRUE(first.has_task());
  EXPECT_EQ(first.task().id(), kTaskId);
  EXPECT_EQ(first.task().status().state(), lf::a2a::v1::TASK_STATE_WORKING);
  EXPECT_EQ(first.task().artifacts_size(), 0);
  EXPECT_EQ(first.task().history_size(), 0);
}

TEST(TaskSubscriptionServiceTest, RejectsTerminalTask) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  EXPECT_FALSE(subscription.ok());
}

TEST(TaskSubscriptionServiceTest, UsesLatestPublishedTaskWhenRegisteringSubscriber) {
  a2a::server::TaskSubscriptionService service;
  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED));

  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());

  const auto first = NextRequired(subscription.value().get());
  ASSERT_TRUE(first.has_task());
  EXPECT_EQ(first.task().status().state(), lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
}

TEST(TaskSubscriptionServiceTest, TimedWaitKeepsSubscriptionOpen) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  auto* session = subscription.value().get();
  (void)NextRequired(session);

  const auto timeout = session->NextFor(kSubscriptionWaitTimeout);
  ASSERT_TRUE(timeout.ok());
  EXPECT_FALSE(timeout.value().has_value());
  EXPECT_TRUE(session->IsLive());

  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED));
  const auto update = NextRequired(session);
  ASSERT_TRUE(update.has_status_update());
  EXPECT_EQ(update.status_update().status().state(), lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
}

TEST(TaskSubscriptionServiceTest, QueuedTerminalEventKeepsSubscriptionLiveUntilDrained) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  (void)NextRequired(subscription.value().get());

  const auto timeout = subscription.value()->NextFor(kSubscriptionWaitTimeout);
  ASSERT_TRUE(timeout.ok());
  ASSERT_FALSE(timeout.value().has_value());

  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  EXPECT_TRUE(subscription.value()->IsLive());

  const auto terminal = NextRequired(subscription.value().get());
  ASSERT_TRUE(terminal.has_status_update());
  EXPECT_EQ(terminal.status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  EXPECT_FALSE(subscription.value()->IsLive());
  ExpectClosed(subscription.value().get());
}

TEST(TaskSubscriptionServiceTest, RejectsStaleSubscriptionAfterTerminalUpdate) {
  a2a::server::TaskSubscriptionService service;
  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_COMPLETED));

  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  EXPECT_FALSE(subscription.ok());
}

TEST(TaskSubscriptionServiceTest, BroadcastsUpdatesToMultipleSubscribersAndClosesOnTerminalState) {
  a2a::server::TaskSubscriptionService service;
  auto first = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  auto second = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  (void)NextRequired(first.value().get());
  (void)NextRequired(second.value().get());

  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED));
  const auto first_update = NextRequired(first.value().get());
  const auto second_update = NextRequired(second.value().get());
  ASSERT_TRUE(first_update.has_status_update());
  ASSERT_TRUE(second_update.has_status_update());
  EXPECT_EQ(first_update.status_update().status().state(), lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
  EXPECT_EQ(second_update.status_update().status().state(), lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);

  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  const auto first_terminal = NextRequired(first.value().get());
  const auto second_terminal = NextRequired(second.value().get());
  ASSERT_TRUE(first_terminal.has_status_update());
  ASSERT_TRUE(second_terminal.has_status_update());
  EXPECT_EQ(first_terminal.status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  EXPECT_EQ(second_terminal.status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  ExpectClosed(first.value().get());
  ExpectClosed(second.value().get());
}

TEST(TaskSubscriptionServiceTest, ConcurrentPublishersPreserveOrderingAcrossSubscribers) {
  a2a::server::TaskSubscriptionService service;
  std::vector<std::unique_ptr<a2a::server::ServerStreamSession>> subscriptions;
  subscriptions.reserve(kOrderingSubscriberCount);
  while (subscriptions.size() < kOrderingSubscriberCount) {
    auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
    ASSERT_TRUE(subscription.ok());
    (void)NextRequired(subscription.value().get());
    subscriptions.push_back(std::move(subscription.value()));
  }

  std::atomic_bool start = false;
  std::vector<std::thread> publishers;
  publishers.reserve(kConcurrentPublisherCount);
  for (std::size_t publisher = 0; publisher < kConcurrentPublisherCount; ++publisher) {
    publishers.emplace_back([publisher, &service, &start] {
      start.wait(false);
      for (std::size_t update = 0; update < kUpdatesPerPublisher; ++update) {
        auto task = MakeTask(lf::a2a::v1::TASK_STATE_WORKING);
        task.set_context_id("publisher-" + std::to_string(publisher) + "-update-" + std::to_string(update));
        service.PublishTaskUpdated(task);
      }
    });
  }

  start.store(true);
  start.notify_all();
  for (auto& publisher : publishers) {
    publisher.join();
  }
  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_COMPLETED));

  const auto expected = DrainStatusContextIds(subscriptions.front().get());
  ASSERT_EQ(expected.size(), (kConcurrentPublisherCount * kUpdatesPerPublisher) + 1);
  for (std::size_t index = 1; index < subscriptions.size(); ++index) {
    EXPECT_EQ(DrainStatusContextIds(subscriptions[index].get()), expected);
  }
}

TEST(TaskSubscriptionServiceTest, RemovingOneSubscriberDoesNotAffectOthers) {
  a2a::server::TaskSubscriptionService service;
  auto first = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  auto second = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  (void)NextRequired(first.value().get());
  (void)NextRequired(second.value().get());
  first.value().reset();

  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  const auto update = NextRequired(second.value().get());
  ASSERT_TRUE(update.has_status_update());
  EXPECT_EQ(update.status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  ExpectClosed(second.value().get());
}

TEST(TaskSubscriptionServiceTest, ConcurrentCancellationIsIdempotent) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  auto* session = subscription.value().get();
  (void)NextRequired(session);

  std::atomic_bool start = false;
  std::vector<std::thread> cancellation_threads;
  cancellation_threads.reserve(kConcurrentCancelThreadCount);
  while (cancellation_threads.size() < kConcurrentCancelThreadCount) {
    cancellation_threads.emplace_back([session, &start] {
      start.wait(false);
      session->Cancel();
    });
  }

  start.store(true);
  start.notify_all();
  for (auto& cancellation_thread : cancellation_threads) {
    cancellation_thread.join();
  }

  EXPECT_FALSE(session->IsLive());
  ExpectClosed(session);
}

TEST(TaskSubscriptionServiceTest, ShutdownClosesActiveSubscriptions) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  (void)NextRequired(subscription.value().get());

  service.Shutdown();

  ExpectClosed(subscription.value().get());
  EXPECT_FALSE(service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING)).ok());
}

TEST(TaskSubscriptionServiceTest, SubscriptionCanOutliveService) {
  std::unique_ptr<a2a::server::ServerStreamSession> session;
  {
    a2a::server::TaskSubscriptionService service;
    auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
    ASSERT_TRUE(subscription.ok());
    session = std::move(subscription.value());
    (void)NextRequired(session.get());
  }

  EXPECT_FALSE(session->IsLive());
  ExpectClosed(session.get());
  session.reset();
}

TEST(TaskSubscriptionServiceTest, PublicationRacingTimeoutDeliversEventExactlyOnce) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  auto* session = subscription.value().get();
  (void)NextRequired(session);
  std::atomic_size_t delivered = 0;

  RunConcurrently(
      [&] {
        const auto next = session->NextFor(kImmediateTimeout);
        ASSERT_TRUE(next.ok());
        delivered.fetch_add(next.value().has_value() ? 1U : 0U);
      },
      [&] { service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED)); });

  const auto remaining = session->NextFor(kSubscriptionWaitTimeout);
  ASSERT_TRUE(remaining.ok());
  delivered.fetch_add(remaining.value().has_value() ? 1U : 0U);
  EXPECT_EQ(delivered.load(), 1U);
}

TEST(TaskSubscriptionServiceTest, PublicationRacingCancellationClosesWithoutDuplicateDelivery) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  auto* session = subscription.value().get();
  (void)NextRequired(session);

  RunConcurrently([&] { service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED)); },
                  [&] { session->Cancel(); });

  EXPECT_LE(DrainStatusUpdateCount(session), kMaximumRacingUpdateCount);
  EXPECT_FALSE(session->IsLive());
}

TEST(TaskSubscriptionServiceTest, ShutdownRacingPublicationClosesWithoutDuplicateDelivery) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  auto* session = subscription.value().get();
  (void)NextRequired(session);

  RunConcurrently([&] { service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED)); },
                  [&] { service.Shutdown(); });

  EXPECT_LE(DrainStatusUpdateCount(session), kMaximumRacingUpdateCount);
  EXPECT_FALSE(session->IsLive());
}

TEST(TaskSubscriptionServiceTest, DestroyingSuspendedSubscriptionPreventsLaterResume) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  (void)NextRequired(subscription.value().get());

  subscription.value().reset();
  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED));

  auto replacement = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(replacement.ok());
  const auto current = NextRequired(replacement.value().get());
  EXPECT_TRUE(current.has_task());
}

TEST(TaskSubscriptionServiceTest, ConcurrentSignalsResumeOnceAndPreserveBothEvents) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  auto* session = subscription.value().get();
  (void)NextRequired(session);
  auto first = MakeTask(lf::a2a::v1::TASK_STATE_WORKING);
  auto second = MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
  first.set_context_id(std::string(kFirstRaceContextId));
  second.set_context_id(std::string(kSecondRaceContextId));

  RunConcurrently([&] { service.PublishTaskUpdated(first); }, [&] { service.PublishTaskUpdated(second); });
  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_COMPLETED));

  const auto contexts = DrainStatusContextIds(session);
  ASSERT_EQ(contexts.size(), kExpectedConcurrentSignalEventCount);
  EXPECT_NE(contexts[0], contexts[1]);
  EXPECT_EQ(contexts.back(), kContextId);
}

}  // namespace
