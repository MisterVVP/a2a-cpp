// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <regex>

#include "a2a/server/server.h"

namespace {

class FailingTaskIdGenerator final : public a2a::server::TaskIdGenerator {
 public:
  [[nodiscard]] a2a::core::Result<std::string> GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                              const a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Internal("boom");
  }
};

TEST(TaskIdGeneratorTest, UuidV7GeneratorProducesPrefixedUuidV7AndUniqueValues) {
  a2a::server::UuidV7TaskIdGenerator generator;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id("m-1");
  a2a::server::RequestContext context;

  const auto first = generator.GenerateTaskId(request, context);
  const auto second = generator.GenerateTaskId(request, context);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_NE(first.value(), second.value());
  EXPECT_TRUE(first.value().rfind("task-", 0) == 0);

  const std::string uuid = first.value().substr(5);
  const std::regex uuid_pattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  EXPECT_TRUE(std::regex_match(uuid, uuid_pattern));
  EXPECT_EQ(uuid[14], '7');
  EXPECT_TRUE(uuid[19] == '8' || uuid[19] == '9' || uuid[19] == 'a' || uuid[19] == 'b');
  EXPECT_LT(first.value(), second.value());
}

TEST(TaskIdGeneratorTest, SequentialTaskIdGeneratorIsDeterministic) {
  a2a::server::SequentialTaskIdGenerator generator;
  lf::a2a::v1::SendMessageRequest request;
  a2a::server::RequestContext context;
  EXPECT_EQ(generator.GenerateTaskId(request, context).value(), "task-test-1");
  EXPECT_EQ(generator.GenerateTaskId(request, context).value(), "task-test-2");
}

TEST(TaskIdGeneratorTest, LifecycleResolveTaskIdValidatesAndPropagatesErrors) {
  a2a::server::InMemoryTaskStore store;
  auto failing = std::make_shared<FailingTaskIdGenerator>();
  a2a::server::TaskLifecycleService lifecycle(&store, failing);
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest no_message_id;
  no_message_id.mutable_message()->add_parts()->set_text("x");
  EXPECT_FALSE(lifecycle.ResolveTaskIdForSendRequest(no_message_id, context).ok());

  lf::a2a::v1::SendMessageRequest generate;
  generate.mutable_message()->set_message_id("id");
  generate.mutable_message()->add_parts()->set_text("x");
  EXPECT_FALSE(lifecycle.ResolveTaskIdForSendRequest(generate, context).ok());
}

TEST(TaskIdGeneratorTest, LifecycleResolveTaskIdPreservesExplicitTaskId) {
  a2a::server::InMemoryTaskStore store;
  lf::a2a::v1::Task task;
  task.set_id("task-existing");
  task.set_context_id("ctx-1");
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  ASSERT_TRUE(store.CreateOrUpdate(task).ok());
  a2a::server::TaskLifecycleService lifecycle(&store);
  a2a::server::RequestContext context;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id("task-existing");
  request.mutable_message()->set_context_id("ctx-1");
  request.mutable_message()->set_message_id("ignored");
  const auto result = lifecycle.ResolveTaskIdForSendRequest(request, context);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), "task-existing");
}

TEST(TaskIdGeneratorTest, LifecycleResolveTaskIdRejectsContextMismatchAndTerminalTask) {
  a2a::server::InMemoryTaskStore store;
  lf::a2a::v1::Task working;
  working.set_id("task-context");
  working.set_context_id("ctx-expected");
  working.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  ASSERT_TRUE(store.CreateOrUpdate(working).ok());
  lf::a2a::v1::Task terminal;
  terminal.set_id("task-terminal");
  terminal.set_context_id("ctx-t");
  terminal.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  ASSERT_TRUE(store.CreateOrUpdate(terminal).ok());

  a2a::server::TaskLifecycleService lifecycle(&store);
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest mismatch;
  mismatch.mutable_message()->set_task_id("task-context");
  mismatch.mutable_message()->set_context_id("ctx-wrong");
  mismatch.mutable_message()->set_message_id("m-1");
  EXPECT_FALSE(lifecycle.ResolveTaskIdForSendRequest(mismatch, context).ok());

  lf::a2a::v1::SendMessageRequest follow_up;
  follow_up.mutable_message()->set_task_id("task-terminal");
  follow_up.mutable_message()->set_context_id("ctx-t");
  follow_up.mutable_message()->set_message_id("m-2");
  EXPECT_FALSE(lifecycle.ResolveTaskIdForSendRequest(follow_up, context).ok());
}

}  // namespace
