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
  send.mutable_message()->set_task_id("unit-example-task");
  const auto send_result = executor.SendMessage(send, context);
  ASSERT_TRUE(send_result.ok());
  EXPECT_EQ(send_result.value().task().id(), "unit-example-task");

  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id("unit-example-task");
  const auto cancel_result = executor.CancelTask(cancel, context);
  ASSERT_TRUE(cancel_result.ok());
  EXPECT_EQ(cancel_result.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

}  // namespace
