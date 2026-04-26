#include <gtest/gtest.h>

#include "a2a/server/server.h"
#include "example_support.h"

namespace {

lf::a2a::v1::StreamResponse RequireNextEvent(a2a::server::ServerStreamSession* stream) {
  EXPECT_NE(stream, nullptr);
  const auto next = stream->Next();
  if (!next.ok()) {
    ADD_FAILURE() << next.error().message();
    return {};
  }
  const auto& maybe_event = next.value();
  if (!maybe_event.has_value()) {
    ADD_FAILURE() << "expected stream event but received end-of-stream";
    return {};
  }
  return maybe_event.value();
}

void ExpectStreamEnded(a2a::server::ServerStreamSession* stream) {
  EXPECT_NE(stream, nullptr);
  const auto end = stream->Next();
  EXPECT_TRUE(end.ok());
  EXPECT_FALSE(end.value().has_value());
}

TEST(ExamplesFunctionalTest, StreamingExecutorReturnsWorkingThenCompletedEvents) {
  constexpr bool kExpectedFinalEvent = true;
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id("functional-stream-task");

  auto stream_result = executor.SendStreamingMessage(request, context);
  ASSERT_TRUE(stream_result.ok()) << stream_result.error().message();
  auto stream = std::move(stream_result.value());

  const auto first = RequireNextEvent(stream.get());
  EXPECT_EQ(first.status_update().status().state(), lf::a2a::v1::TASK_STATE_WORKING);

  const auto second = RequireNextEvent(stream.get());
  EXPECT_EQ(second.status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  EXPECT_EQ(second.status_update().final(), kExpectedFinalEvent);

  ExpectStreamEnded(stream.get());
}

}  // namespace
