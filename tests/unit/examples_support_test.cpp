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
  EXPECT_EQ(send_result.value().task().id(), "task-unit-example-task");

  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id("task-unit-example-task");
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
  EXPECT_EQ(first_event->status_update().status().state(), lf::a2a::v1::TASK_STATE_WORKING);

  const auto second = stream_result.value()->Next();
  ASSERT_TRUE(second.ok());
  const auto& second_event = second.value();
  ASSERT_TRUE(second_event.has_value());
  EXPECT_EQ(second_event->status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);

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
    send.mutable_message()->set_task_id("task-history-task");
    send.mutable_message()->add_parts()->set_text("entry");
    ASSERT_TRUE(executor.SendMessage(send, context).ok());
  }

  lf::a2a::v1::GetTaskRequest get;
  get.set_id("task-history-task");
  get.set_history_length(1);
  const auto loaded = executor.GetTask(get, context);
  ASSERT_TRUE(loaded.ok());
  EXPECT_EQ(loaded.value().history_size(), 1);
}

}  // namespace
