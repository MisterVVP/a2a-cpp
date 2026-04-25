#include <gtest/gtest.h>

#include "a2a/server/server.h"
#include "example_support.h"

namespace {

TEST(ExamplesFunctionalTest, StreamingExecutorReturnsWorkingThenCompletedEvents) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id("functional-stream-task");

  auto stream_result = executor.SendStreamingMessage(request, context);
  ASSERT_TRUE(stream_result.ok()) << stream_result.error().message();

  auto stream = std::move(stream_result.value());
  const auto first = stream->Next();
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(first.value().has_value());
  EXPECT_EQ(first.value()->status_update().status().state(), lf::a2a::v1::TASK_STATE_WORKING);

  const auto second = stream->Next();
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(second.value().has_value());
  EXPECT_EQ(second.value()->status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  EXPECT_TRUE(second.value()->status_update().final());

  const auto end = stream->Next();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end.value().has_value());
}

}  // namespace
