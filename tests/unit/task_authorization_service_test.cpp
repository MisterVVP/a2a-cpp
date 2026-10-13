// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/task_authorization_service.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "a2a/server/tasks/in_memory_task_store.h"
#include "support/security/fixtures.h"

namespace {
using namespace a2a::server;
using namespace a2a::tests::security;
constexpr std::string_view kAlice = "alice";
constexpr std::string_view kBob = "bob";
constexpr std::string_view kMessageId = "authorization-message";
constexpr std::string_view kFollowupId = "authorization-followup";
constexpr std::string_view kText = "authorization text";
constexpr int kHistoryCount = 2;

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
  InMemoryTaskStore store_;
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
TEST_F(TaskAuthorizationServiceTest, RejectsMissingExplanationsAndUnconfiguredStore) {
  EXPECT_FALSE(service_.RequestAuthorization(Message(), RequestContext{}, {}).ok());
  auto missing_id = explanation_;
  missing_id.clear_message_id();
  EXPECT_FALSE(service_.RequestAuthorization(Message(), RequestContext{}, missing_id).ok());
  TaskAuthorizationService unconfigured(nullptr, {});
  EXPECT_FALSE(unconfigured.RequestAuthorization(Message(), RequestContext{}, explanation_).ok());
  EXPECT_FALSE(unconfigured.Subscribe(id_).ok());
}
}  // namespace
