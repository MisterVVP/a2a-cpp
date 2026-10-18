// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/task_authorization_service.h"

#include <gtest/gtest.h>

#include <future>
#include <string>
#include <thread>
#include <vector>

#include "a2a/server/tasks/in_memory_task_store.h"
#include "support/security/fixtures.h"
#include "support/security/observed_task_store.h"

namespace {
using namespace a2a::server;
using namespace a2a::tests::security;
constexpr std::string_view kAlice = "alice";
constexpr std::string_view kBob = "bob";
constexpr std::string_view kMessageId = "authorization-message";
constexpr std::string_view kFollowupId = "authorization-followup";
constexpr std::string_view kText = "authorization text";
constexpr int kHistoryCount = 2;
constexpr std::string_view kOrdinaryTaskId = "ordinary-task";

lf::a2a::v1::SendMessageRequest Message(std::string_view id = kMessageId) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id(std::string(id));
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_context_id(std::string(kAlice));
  request.mutable_message()->add_parts()->set_text(std::string(kText));
  return request;
}

class TaskAuthorizationServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    explanation_.set_role(lf::a2a::v1::ROLE_AGENT);
    explanation_.set_message_id(std::string(kMessageId));
    explanation_.add_parts()->set_text(std::string(kText));
    auto task = service_.RequestAuthorization(Message(), RequestContext{}, explanation_);
    ASSERT_TRUE(task.ok());
    id_ = task.value().id();
  }
  [[nodiscard]] lf::a2a::v1::SendMessageRequest Followup() const {
    auto request = Message(kFollowupId);
    request.mutable_message()->set_task_id(id_);
    return request;
  }
  static a2a::core::Result<std::vector<lf::a2a::v1::TaskState>> Drain(ServerStreamSession& stream) {
    std::vector<lf::a2a::v1::TaskState> states;
    while (true) {
      auto next = stream.Next();
      if (!next.ok()) {
        return next.error();
      }
      const auto& item = next.value();
      if (!item.has_value()) {
        return states;
      }
      if (item->has_status_update()) {
        states.push_back(item->status_update().status().state());
      }
    }
  }
  void AuthorizeAndComplete() {
    ASSERT_TRUE(service_.Authorize(id_, kTaskCredential).ok());
    ASSERT_TRUE(service_.Complete(id_).ok());
  }
  ObservedTaskStore store_;
  TaskAuthorizationService service_{
      &store_, [](const auto&, std::string_view credential) { return credential == kTaskCredential; }};
  lf::a2a::v1::Message explanation_;
  std::string id_;
};
TEST_F(TaskAuthorizationServiceTest, PersistsAnExplanatoryAuthorizationPendingTask) {
  auto task = store_.Get(id_);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().status().state(), lf::a2a::v1::TASK_STATE_AUTH_REQUIRED);
  EXPECT_FALSE(task.value().status().message().parts().empty());
  EXPECT_TRUE(TaskAuthorizationService::IsAuthorizationTask(task.value()));
}
TEST_F(TaskAuthorizationServiceTest, IncomingMessagesPreservePendingStateAndHistory) {
  auto in_band = Followup();
  in_band.mutable_message()->mutable_parts(0)->set_text(std::string(kTaskCredential));
  auto received = service_.ReceiveMessage(in_band);
  ASSERT_TRUE(received.ok());
  EXPECT_EQ(received.value().history_size(), kHistoryCount);
  EXPECT_EQ(received.value().status().state(), lf::a2a::v1::TASK_STATE_AUTH_REQUIRED);
  auto duplicate = service_.ReceiveMessage(in_band);
  ASSERT_TRUE(duplicate.ok());
  EXPECT_EQ(duplicate.value().history_size(), kHistoryCount);
}
TEST_F(TaskAuthorizationServiceTest, InvalidCredentialsCannotResumeOrCompleteTask) {
  EXPECT_FALSE(service_.Complete(id_).ok());
  EXPECT_FALSE(service_.Authorize(id_, kUnknownToken).ok());
  auto task = store_.Get(id_);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().status().state(), lf::a2a::v1::TASK_STATE_AUTH_REQUIRED);
}
TEST_F(TaskAuthorizationServiceTest, StreamsRemainLiveAndReceiveAuthorizationAndCompletion) {
  auto subscribed = service_.Subscribe(id_);
  ASSERT_TRUE(subscribed.ok());
  auto initial = subscribed.value()->Next();
  ASSERT_TRUE(initial.ok());
  EXPECT_TRUE(initial.value().has_value());
  EXPECT_TRUE(subscribed.value()->IsLive());
  AuthorizeAndComplete();
  auto states = Drain(*subscribed.value());
  ASSERT_TRUE(states.ok());
  ASSERT_FALSE(states.value().empty());
  EXPECT_EQ(states.value().back(), lf::a2a::v1::TASK_STATE_COMPLETED);
}
TEST_F(TaskAuthorizationServiceTest, RejectsMismatchedContextAndAllowsCancellationWhilePending) {
  auto wrong = Followup();
  wrong.mutable_message()->set_context_id(std::string(kBob));
  EXPECT_FALSE(service_.ReceiveMessage(wrong).ok());
  auto canceled = service_.Cancel(id_);
  ASSERT_TRUE(canceled.ok());
  EXPECT_EQ(canceled.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
  EXPECT_FALSE(service_.Authorize(id_, kTaskCredential).ok());
}

TEST_F(TaskAuthorizationServiceTest, CancellationRejectsOrdinaryTasksWithoutWriting) {
  lf::a2a::v1::Task task;
  task.set_id(std::string(kOrdinaryTaskId));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  ASSERT_TRUE(store_.CreateOrUpdate(task).ok());
  const auto writes = store_.conditional_writes;
  EXPECT_FALSE(service_.Cancel(kOrdinaryTaskId).ok());
  EXPECT_EQ(store_.conditional_writes, writes);
  auto stored = store_.Get(kOrdinaryTaskId);
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored.value().status().state(), lf::a2a::v1::TASK_STATE_WORKING);
}
TEST_F(TaskAuthorizationServiceTest, RejectsMissingExplanationsAndUnconfiguredStore) {
  EXPECT_FALSE(service_.RequestAuthorization(Message(), RequestContext{}, {}).ok());
  auto missing_id = explanation_;
  missing_id.clear_message_id();
  EXPECT_FALSE(service_.RequestAuthorization(Message(), RequestContext{}, missing_id).ok());
  TaskAuthorizationService unconfigured(nullptr, {});
  EXPECT_FALSE(unconfigured.RequestAuthorization(Message(), RequestContext{}, explanation_).ok());
  EXPECT_FALSE(unconfigured.Subscribe(id_).ok());
}

constexpr auto kReadTimeout = std::chrono::milliseconds(1);
constexpr std::size_t kOneWrite = 1;

TEST_F(TaskAuthorizationServiceTest, FailedPersistenceLeavesPendingTaskAndStreamUnchanged) {
  auto stream = service_.Subscribe(id_);
  ASSERT_TRUE(stream.ok());
  ASSERT_TRUE(stream.value()->Next().ok());
  const auto writes = store_.conditional_writes;
  store_.fail_next_write = true;
  EXPECT_FALSE(service_.Authorize(id_, kTaskCredential).ok());
  auto pending = store_.Get(id_);
  ASSERT_TRUE(pending.ok());
  EXPECT_EQ(pending.value().status().state(), lf::a2a::v1::TASK_STATE_AUTH_REQUIRED);
  EXPECT_TRUE(pending.value().status().has_message());
  auto event = stream.value()->NextFor(kReadTimeout);
  ASSERT_TRUE(event.ok());
  EXPECT_FALSE(event.value().has_value());
  EXPECT_EQ(store_.conditional_writes, writes + kOneWrite);
  EXPECT_TRUE(service_.Authorize(id_, kTaskCredential).ok());
  auto working = store_.Get(id_);
  ASSERT_TRUE(working.ok());
  EXPECT_FALSE(working.value().status().has_message());
  EXPECT_TRUE(service_.Complete(id_).ok());
}

TEST_F(TaskAuthorizationServiceTest, ConcurrentCancellationCannotBeOverwrittenByAuthorization) {
  store_.before_write = [&] { EXPECT_TRUE(store_.backing.Cancel(id_).ok()); };
  EXPECT_FALSE(service_.Authorize(id_, kTaskCredential).ok());
  auto task = store_.Get(id_);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
  EXPECT_FALSE(service_.Authorize(id_, kTaskCredential).ok());
}

TEST_F(TaskAuthorizationServiceTest, FailedCompletionCanBeRetriedWithoutLosingWorkingState) {
  ASSERT_TRUE(service_.Authorize(id_, kTaskCredential).ok());
  store_.fail_next_write = true;
  EXPECT_FALSE(service_.Complete(id_).ok());
  auto task = store_.Get(id_);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().status().state(), lf::a2a::v1::TASK_STATE_WORKING);
  EXPECT_TRUE(service_.Complete(id_).ok());
}

std::thread StartCredentials(TaskAuthorizationService& service, std::string id, std::promise<void>& ready) {
  return std::thread([&service, &ready, id = std::move(id)] {
    ready.set_value();
    EXPECT_TRUE(service.Authorize(id, kTaskCredential).ok());
    EXPECT_TRUE(service.Complete(id).ok());
  });
}

void RegisterImmediateCredentials(ObservedTaskStore& store, TaskAuthorizationService& service, std::thread& credentials,
                                  std::promise<void>& ready) {
  auto ready_future = ready.get_future().share();
  store.after_write = [&, ready_future, started = false](const lf::a2a::v1::Task& task) mutable {
    if (started) {
      return;
    }
    started = true;
    credentials = StartCredentials(service, task.id(), ready);
    ready_future.wait();
  };
}

void CheckInitialAuthorization(ServerStreamSession& stream) {
  auto initial = stream.Next();
  ASSERT_TRUE(initial.ok());
  const auto& event = initial.value();
  ASSERT_TRUE(event.has_value());
  const auto snapshot = event.value_or(lf::a2a::v1::StreamResponse{});
  EXPECT_EQ(snapshot.task().status().state(), lf::a2a::v1::TASK_STATE_AUTH_REQUIRED);
}

TEST_F(TaskAuthorizationServiceTest, ImmediateCredentialArrivalKeepsSendStreamRegistered) {
  std::promise<void> ready;
  std::thread credentials;
  RegisterImmediateCredentials(store_, service_, credentials, ready);
  auto stream = service_.SendStreamingMessage(Message(kFollowupId), RequestContext{}, explanation_);
  credentials.join();
  ASSERT_TRUE(stream.ok());
  CheckInitialAuthorization(*stream.value());
  auto states = Drain(*stream.value());
  ASSERT_TRUE(states.ok());
  ASSERT_FALSE(states.value().empty());
  EXPECT_EQ(states.value().front(), lf::a2a::v1::TASK_STATE_WORKING);
  EXPECT_EQ(states.value().back(), lf::a2a::v1::TASK_STATE_COMPLETED);
}

}  // namespace
