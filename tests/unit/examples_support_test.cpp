// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include "a2a/server/server.h"
#include "example_support.h"

namespace {

TEST(ExampleSupportTest, UrlToTargetExtractsPath) {
  EXPECT_EQ(a2a::examples::UrlToTarget("http://agent.local/a2a/tasks"), "/a2a/tasks");
  EXPECT_EQ(a2a::examples::UrlToTarget("https://agent.local"), "/");
  EXPECT_EQ(a2a::examples::UrlToTarget("/already/path"), "/already/path");
}

TEST(ExampleSupportTest, ExampleExecutorHandlesSendAndCancelFlow) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_message_id("unit-example-task");
  send.mutable_message()->add_parts()->set_text("hello");
  const auto send_result = executor.SendMessage(send, context);
  ASSERT_TRUE(send_result.ok());
  EXPECT_EQ(send_result.value().task().id(), "task-test-1");

  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id("task-test-1");
  const auto cancel_result = executor.CancelTask(cancel, context);
  ASSERT_TRUE(cancel_result.ok());
  EXPECT_EQ(cancel_result.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

TEST(ExampleSupportTest, StreamingAndListTasksAreDeterministic) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_message_id("stream-case");
  send.mutable_message()->add_parts()->set_text("hello stream");
  ASSERT_TRUE(executor.SendMessage(send, context).ok());

  lf::a2a::v1::SendMessageRequest stream_request;
  stream_request.mutable_message()->set_message_id("stream-case");
  stream_request.mutable_message()->add_parts()->set_text("hello stream");
  const auto stream_result = executor.SendStreamingMessage(stream_request, context);
  ASSERT_TRUE(stream_result.ok());

  const auto first = stream_result.value()->Next();
  ASSERT_TRUE(first.ok());
  const auto& first_event = first.value();
  ASSERT_TRUE(first_event.has_value());
  EXPECT_EQ(first_event.value_or(lf::a2a::v1::StreamResponse{}).status_update().status().state(),
            lf::a2a::v1::TASK_STATE_WORKING);

  const auto second = stream_result.value()->Next();
  ASSERT_TRUE(second.ok());
  const auto& second_event = second.value();
  ASSERT_TRUE(second_event.has_value());
  EXPECT_EQ(second_event.value_or(lf::a2a::v1::StreamResponse{}).status_update().status().state(),
            lf::a2a::v1::TASK_STATE_COMPLETED);

  const auto done = stream_result.value()->Next();
  ASSERT_TRUE(done.ok());
  EXPECT_FALSE(done.value().has_value());

  const auto listed = executor.ListTasks({}, context);
  ASSERT_TRUE(listed.ok());
  ASSERT_FALSE(listed.value().tasks.empty());
}

TEST(ExampleSupportTest, SendMessageRequiresAtLeastOnePart) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;
  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_message_id("invalid");
  const auto result = executor.SendMessage(send, context);
  ASSERT_FALSE(result.ok());
}

TEST(ExampleSupportTest, GetTaskWithHistoryLengthFiltersHistory) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest first;
  first.mutable_message()->set_message_id("history-task");
  first.mutable_message()->add_parts()->set_text("entry");
  ASSERT_TRUE(executor.SendMessage(first, context).ok());

  for (int i = 0; i < 2; ++i) {
    lf::a2a::v1::SendMessageRequest send;
    send.mutable_message()->set_message_id("h" + std::to_string(i));
    send.mutable_message()->set_task_id("task-test-1");
    send.mutable_message()->add_parts()->set_text("entry");
    ASSERT_TRUE(executor.SendMessage(send, context).ok());
  }

  lf::a2a::v1::GetTaskRequest get;
  get.set_id("task-test-1");
  get.set_history_length(1);
  const auto loaded = executor.GetTask(get, context);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded.value().history_size(), 1);
}

TEST(ExampleSupportTest, SendMessageRejectsTerminalTaskFollowup) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest create;
  create.mutable_message()->set_message_id("complete-task-case");
  create.mutable_message()->add_parts()->set_text("complete task");
  const auto created = executor.SendMessage(create, context);
  ASSERT_TRUE(created.ok());
  ASSERT_TRUE(created.value().has_task());
  EXPECT_EQ(created.value().task().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  const std::string created_task_id = created.value().task().id();

  lf::a2a::v1::SendMessageRequest followup;
  followup.mutable_message()->set_message_id("followup-message");
  followup.mutable_message()->set_task_id(created_task_id);
  followup.mutable_message()->add_parts()->set_text("another message");
  const auto result = executor.SendMessage(followup, context);
  ASSERT_FALSE(result.ok());
}

TEST(ExampleSupportTest, StreamingWithoutIdsIsDeterministicAcrossSessions) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest stream_request;
  stream_request.mutable_message()->add_parts()->set_text("no ids");

  const auto stream_a = executor.SendStreamingMessage(stream_request, context);
  const auto stream_b = executor.SendStreamingMessage(stream_request, context);
  ASSERT_TRUE(stream_a.ok());
  ASSERT_TRUE(stream_b.ok());

  const auto first_a = stream_a.value()->Next();
  const auto first_b = stream_b.value()->Next();
  ASSERT_TRUE(first_a.ok());
  ASSERT_TRUE(first_b.ok());
  const auto& first_a_maybe = first_a.value();
  const auto& first_b_maybe = first_b.value();
  ASSERT_TRUE(first_a_maybe.has_value());
  ASSERT_TRUE(first_b_maybe.has_value());
  const lf::a2a::v1::StreamResponse empty_event;
  const auto& first_a_status = first_a_maybe.value_or(empty_event).status_update();
  const auto& first_b_status = first_b_maybe.value_or(empty_event).status_update();
  EXPECT_EQ(first_a_status.task_id(), first_b_status.task_id());
  EXPECT_EQ(first_a_status.context_id(), first_b_status.context_id());
}

}  // namespace
