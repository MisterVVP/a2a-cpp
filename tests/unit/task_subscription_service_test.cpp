#include "a2a/server/task_subscription_service.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

namespace {

constexpr std::string_view kTaskId = "subscription-task";
constexpr std::string_view kContextId = "subscription-context";
constexpr std::string_view kTransientArtifactId = "transient-artifact";
constexpr std::string_view kTransientHistoryMessageId = "transient-history-message";
constexpr std::chrono::milliseconds kSubscriptionWaitTimeout{1};

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
  (void)NextRequired(subscription.value().get());

  const auto timeout = subscription.value()->NextFor(kSubscriptionWaitTimeout);
  ASSERT_TRUE(timeout.ok());
  EXPECT_FALSE(timeout.value().has_value());
  EXPECT_TRUE(subscription.value()->IsLive());

  service.PublishTaskUpdated(MakeTask(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED));
  const auto update = NextRequired(subscription.value().get());
  ASSERT_TRUE(update.has_status_update());
  EXPECT_EQ(update.status_update().status().state(), lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
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

TEST(TaskSubscriptionServiceTest, ShutdownClosesActiveSubscriptions) {
  a2a::server::TaskSubscriptionService service;
  auto subscription = service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING));
  ASSERT_TRUE(subscription.ok());
  (void)NextRequired(subscription.value().get());

  service.Shutdown();

  ExpectClosed(subscription.value().get());
  EXPECT_FALSE(service.Subscribe(MakeTask(lf::a2a::v1::TASK_STATE_WORKING)).ok());
}

}  // namespace
