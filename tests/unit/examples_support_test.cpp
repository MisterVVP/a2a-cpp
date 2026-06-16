// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/server/request_context.h"
#include "a2a/server/task_id_generator.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"
#include "example_support.h"

namespace {

class FixedTaskIdGenerator final : public a2a::server::TaskIdGenerator {
 public:
  explicit FixedTaskIdGenerator(std::string task_id) : task_id_(std::move(task_id)) {}

  [[nodiscard]] a2a::core::Result<std::string> GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                              const a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return task_id_;
  }

 private:
  std::string task_id_;
};

class FailingGetTaskStore final : public a2a::server::TaskStore {
 public:
  explicit FailingGetTaskStore(a2a::core::Error error) : error_(std::move(error)) {}

  [[nodiscard]] a2a::core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override {
    (void)task;
    ++create_or_update_calls_;
    return {};
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override {
    (void)id;
    return error_;
  }

  [[nodiscard]] a2a::core::Result<a2a::server::ListTasksResponse> List(
      const a2a::server::ListTasksRequest& request) const override {
    (void)request;
    return a2a::core::Error::Internal("list failed");
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override {
    (void)id;
    return error_;
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                       const lf::a2a::v1::Message& message,
                                                                       HistoryAppendPolicy policy) override {
    (void)task_id;
    (void)message;
    (void)policy;
    return error_;
  }

  [[nodiscard]] HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override { return {}; }

  [[nodiscard]] int create_or_update_calls() const noexcept { return create_or_update_calls_; }

 private:
  a2a::core::Error error_;
  int create_or_update_calls_ = 0;
};

[[nodiscard]] a2a::examples::ExampleExecutor MakeExecutorWithTaskId(std::string task_id) {
  a2a::examples::ExampleExecutorOptions options;
  options.task_id_generator = std::make_shared<FixedTaskIdGenerator>(std::move(task_id));
  return a2a::examples::ExampleExecutor(std::move(options));
}

[[nodiscard]] lf::a2a::v1::SendMessageRequest MakeValidSendRequest(std::string message_id) {
  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_message_id(std::move(message_id));
  send.mutable_message()->add_parts()->set_text("hello");
  return send;
}

TEST(ExampleSupportTest, UrlToTargetExtractsNormalizedPathOnly) {
  EXPECT_EQ(a2a::examples::UrlToTarget("http://agent.local/a2a/tasks?limit=1#frag"), "/a2a/tasks");
  EXPECT_EQ(a2a::examples::UrlToTarget("https://agent.local"), "/");
  EXPECT_EQ(a2a::examples::UrlToTarget("already/path"), "/already/path");
}

TEST(ExampleSupportTest, ExampleExecutorHandlesSendAndCancelFlow) {
  auto executor = MakeExecutorWithTaskId("task-test-1");
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

TEST(ExampleSupportTest, SendMessagePropagatesInjectedStoreReadErrors) {
  FailingGetTaskStore task_store(a2a::core::Error::Internal("read failed"));
  a2a::examples::ExampleExecutorOptions options;
  options.task_store = &task_store;
  options.task_id_generator = std::make_shared<FixedTaskIdGenerator>("task-test-read-error");
  a2a::examples::ExampleExecutor executor(std::move(options));
  a2a::server::RequestContext context;

  const auto result = executor.SendMessage(MakeValidSendRequest("read-error-send"), context);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kInternal);
  EXPECT_EQ(result.error().message(), "read failed");
  EXPECT_EQ(task_store.create_or_update_calls(), 0);
}

TEST(ExampleSupportTest, StreamingPropagatesInjectedStoreReadErrors) {
  FailingGetTaskStore task_store(a2a::core::Error::Internal("stream read failed"));
  a2a::examples::ExampleExecutorOptions options;
  options.task_store = &task_store;
  options.task_id_generator = std::make_shared<FixedTaskIdGenerator>("task-test-stream-read-error");
  a2a::examples::ExampleExecutor executor(std::move(options));
  a2a::server::RequestContext context;

  const auto result = executor.SendStreamingMessage(MakeValidSendRequest("read-error-stream"), context);

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kInternal);
  EXPECT_EQ(result.error().message(), "stream read failed");
  EXPECT_EQ(task_store.create_or_update_calls(), 0);
}

TEST(ExampleSupportTest, GetTaskWithHistoryLengthFiltersHistory) {
  auto executor = MakeExecutorWithTaskId("task-test-1");
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

TEST(ExampleSupportTest, MessageIdPrefixesDriveArtifactAndMessageResponse) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_message_id("message-response-artifact-file-url");
  send.mutable_message()->add_parts()->set_text("plain text without heuristic keywords");
  const auto response = executor.SendMessage(send, context);

  ASSERT_TRUE(response.ok());
  ASSERT_TRUE(response.value().has_message());
  EXPECT_FALSE(response.value().has_task());
  EXPECT_EQ(response.value().message().parts(0).text(), "Direct message response");
}

TEST(ExampleSupportTest, MessageIdPrefixesDriveTaskTerminalStates) {
  a2a::examples::ExampleExecutor executor;
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest completed;
  completed.mutable_message()->set_message_id("artifact-data-complete-task");
  completed.mutable_message()->add_parts()->set_text("no keywords");
  const auto completed_result = executor.SendMessage(completed, context);

  ASSERT_TRUE(completed_result.ok());
  ASSERT_TRUE(completed_result.value().has_task());
  EXPECT_EQ(completed_result.value().task().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);

  lf::a2a::v1::SendMessageRequest input_required;
  input_required.mutable_message()->set_message_id("artifact-file-input-required");
  input_required.mutable_message()->add_parts()->set_text("no keywords");
  const auto input_required_result = executor.SendMessage(input_required, context);

  ASSERT_TRUE(input_required_result.ok());
  ASSERT_TRUE(input_required_result.value().has_task());
  EXPECT_EQ(input_required_result.value().task().status().state(), lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
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
  auto first_a_event = first_a_maybe.value_or(lf::a2a::v1::StreamResponse{});
  auto first_b_event = first_b_maybe.value_or(lf::a2a::v1::StreamResponse{});
  EXPECT_EQ(first_a_event.status_update().task_id(), first_b_event.status_update().task_id());
  EXPECT_EQ(first_a_event.status_update().context_id(), first_b_event.status_update().context_id());
}

}  // namespace
