// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "a2a/server/request_context.h"
#include "a2a/server/task_id_generator.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"
#include "example_support.h"

namespace {

constexpr std::string_view kSingleUpsertTaskId = "single-upsert-task";
constexpr std::string_view kSingleUpsertMessageId = "single-upsert-message";
constexpr std::string_view kDuplicateTaskId = "duplicate-task";
constexpr std::string_view kDuplicateCreateMessageId = "duplicate-create";
constexpr std::string_view kDuplicateMessageId = "duplicate-message";
constexpr std::string_view kCrossExecutorTaskId = "cross-executor-task";
constexpr std::string_view kCrossExecutorCreateMessageId = "cross-executor-create";
constexpr std::string_view kCrossExecutorFirstMessageId = "cross-executor-first";
constexpr std::string_view kCrossExecutorSecondMessageId = "cross-executor-second";
constexpr std::string_view kCollisionContextId = "collision-context";
constexpr std::string_view kSingleSnapshotFollowUpMessageId = "single-snapshot-follow-up";
constexpr std::string_view kMissingFollowUpMessageId = "missing-follow-up";
constexpr std::string_view kMissingFollowUpTaskId = "missing-task";
constexpr std::string_view kMismatchedFollowUpMessageId = "mismatched-follow-up";
constexpr std::string_view kMismatchedFollowUpContextId = "different-context";
constexpr std::size_t kSingleHistoryEntry = 1U;
constexpr std::size_t kDuplicateHistorySize = 2U;
constexpr std::size_t kCrossExecutorHistorySize = 3U;
constexpr int kSingleUpsertCallCount = 1;
constexpr int kDuplicateTotalUpsertCount = 3;
constexpr int kNoStoreCalls = 0;
constexpr int kCollisionReadCount = 1;
constexpr int kCollisionWriteCount = 2;
constexpr std::string_view kCrossExecutorResultNotPopulated = "cross-executor result not populated";

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

class BlockingGetTaskStore final : public a2a::server::TaskStore {
 public:
  explicit BlockingGetTaskStore(std::string blocked_task_id) : blocked_task_id_(std::move(blocked_task_id)) {}

  [[nodiscard]] a2a::core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override {
    return store_.CreateOrUpdate(task);
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override {
    if (id == blocked_task_id_) {
      std::unique_lock lock(mutex_);
      blocked_ = true;
      condition_.notify_all();
      condition_.wait(lock, [&] { return released_; });
    }
    return store_.Get(id);
  }

  [[nodiscard]] a2a::core::Result<a2a::server::ListTasksResponse> List(
      const a2a::server::ListTasksRequest& request) const override {
    return store_.List(request);
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override { return store_.Cancel(id); }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                       const lf::a2a::v1::Message& message,
                                                                       HistoryAppendPolicy policy) override {
    return store_.AppendTaskHistory(task_id, message, policy);
  }

  [[nodiscard]] HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override {
    return store_.GetHistoryTelemetrySnapshot();
  }

  void WaitUntilBlocked() const {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [&] { return blocked_; });
  }

  void Release() {
    {
      std::lock_guard lock(mutex_);
      released_ = true;
    }
    condition_.notify_all();
  }

 private:
  std::string blocked_task_id_;
  a2a::server::InMemoryTaskStore store_;
  mutable std::mutex mutex_;
  mutable std::condition_variable condition_;
  mutable bool blocked_ = false;
  bool released_ = false;
};

class CountingTaskStore final : public a2a::server::TaskStore {
 public:
  [[nodiscard]] a2a::core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override {
    ++upsert_calls_;
    last_upsert_ = task;
    return store_.CreateOrUpdate(task);
  }

  [[nodiscard]] bool SupportsConditionalWrites() const noexcept override { return true; }

  [[nodiscard]] a2a::core::Result<TaskSnapshot> GetSnapshot(std::string_view id) const override {
    ++get_calls_;
    return store_.GetSnapshot(id);
  }

  [[nodiscard]] a2a::core::Result<ConditionalWriteResult> CreateOrUpdateIfRevision(
      const lf::a2a::v1::Task& task, std::uint64_t expected_revision) override {
    ++upsert_calls_;
    last_upsert_ = task;
    return store_.CreateOrUpdateIfRevision(task, expected_revision);
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override {
    ++get_calls_;
    return store_.Get(id);
  }

  [[nodiscard]] a2a::core::Result<a2a::server::ListTasksResponse> List(
      const a2a::server::ListTasksRequest& request) const override {
    return store_.List(request);
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override { return store_.Cancel(id); }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                       const lf::a2a::v1::Message& message,
                                                                       HistoryAppendPolicy policy) override {
    ++append_calls_;
    return store_.AppendTaskHistory(task_id, message, policy);
  }

  [[nodiscard]] HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override {
    return store_.GetHistoryTelemetrySnapshot();
  }

  [[nodiscard]] int get_calls() const noexcept { return get_calls_; }
  [[nodiscard]] int upsert_calls() const noexcept { return upsert_calls_; }
  [[nodiscard]] int append_calls() const noexcept { return append_calls_; }
  [[nodiscard]] const lf::a2a::v1::Task& last_upsert() const noexcept { return last_upsert_; }
  void ResetCounts() noexcept {
    get_calls_ = 0;
    upsert_calls_ = 0;
    append_calls_ = 0;
  }

 private:
  a2a::server::InMemoryTaskStore store_;
  mutable int get_calls_ = 0;
  int upsert_calls_ = 0;
  int append_calls_ = 0;
  lf::a2a::v1::Task last_upsert_;
};

class CoordinatedConditionalTaskStore final : public a2a::server::TaskStore {
 public:
  [[nodiscard]] a2a::core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override {
    return store_.CreateOrUpdate(task);
  }
  [[nodiscard]] bool SupportsConditionalWrites() const noexcept override { return true; }
  [[nodiscard]] a2a::core::Result<TaskSnapshot> GetSnapshot(std::string_view id) const override {
    auto snapshot = store_.GetSnapshot(id);
    if (coordinate_snapshots_ && coordinated_snapshot_count_.fetch_add(1) < 2) {
      snapshot_barrier_.arrive_and_wait();
    }
    return snapshot;
  }
  [[nodiscard]] a2a::core::Result<ConditionalWriteResult> CreateOrUpdateIfRevision(
      const lf::a2a::v1::Task& task, std::uint64_t expected_revision) override {
    return store_.CreateOrUpdateIfRevision(task, expected_revision);
  }
  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override { return store_.Get(id); }
  [[nodiscard]] a2a::core::Result<a2a::server::ListTasksResponse> List(
      const a2a::server::ListTasksRequest& request) const override {
    return store_.List(request);
  }
  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override { return store_.Cancel(id); }
  [[nodiscard]] a2a::core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                       const lf::a2a::v1::Message& message,
                                                                       HistoryAppendPolicy policy) override {
    ++append_calls_;
    return store_.AppendTaskHistory(task_id, message, policy);
  }
  [[nodiscard]] HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override {
    return store_.GetHistoryTelemetrySnapshot();
  }
  void CoordinateNextTwoSnapshots() noexcept { coordinate_snapshots_ = true; }
  [[nodiscard]] int append_calls() const noexcept { return append_calls_; }

 private:
  a2a::server::InMemoryTaskStore store_;
  mutable std::barrier<> snapshot_barrier_{2};
  mutable std::atomic<int> coordinated_snapshot_count_ = 0;
  mutable bool coordinate_snapshots_ = false;
  int append_calls_ = 0;
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

[[nodiscard]] std::array<bool, 2> RunConcurrentFollowUps(a2a::examples::ExampleExecutor& executor,
                                                         std::string_view task_id,
                                                         const std::array<std::string_view, 2>& message_ids) {
  std::barrier start(2);
  std::array<bool, 2> succeeded{};
  const auto send_follow_up = [&](std::size_t index) {
    lf::a2a::v1::SendMessageRequest request = MakeValidSendRequest(std::string(message_ids[index]));
    request.mutable_message()->set_task_id(std::string(task_id));
    a2a::server::RequestContext context;
    start.arrive_and_wait();
    succeeded[index] = executor.SendMessage(request, context).ok();
  };
  std::thread first(send_follow_up, 0U);
  std::thread second(send_follow_up, 1U);
  first.join();
  second.join();
  return succeeded;
}

[[nodiscard]] bool TaskContainsMessage(const lf::a2a::v1::Task& task, std::string_view message_id) {
  return std::ranges::any_of(task.history(),
                             [&](const lf::a2a::v1::Message& message) { return message.message_id() == message_id; });
}

struct CrossExecutorResult final {
  std::array<bool, 2> succeeded{};
  a2a::core::Result<lf::a2a::v1::Task> stored_task =
      a2a::core::Error::Internal(std::string(kCrossExecutorResultNotPopulated));
  int append_calls = 0;
};

[[nodiscard]] CrossExecutorResult RunCrossExecutorConflictScenario() {
  CoordinatedConditionalTaskStore task_store;
  a2a::examples::ExampleExecutorOptions first_options;
  first_options.task_store = &task_store;
  first_options.task_id_generator = std::make_shared<FixedTaskIdGenerator>(std::string(kCrossExecutorTaskId));
  a2a::examples::ExampleExecutor first_executor(std::move(first_options));
  a2a::examples::ExampleExecutorOptions second_options;
  second_options.task_store = &task_store;
  second_options.task_id_generator = std::make_shared<FixedTaskIdGenerator>(std::string(kCrossExecutorTaskId));
  a2a::examples::ExampleExecutor second_executor(std::move(second_options));
  a2a::server::RequestContext create_context;
  if (!first_executor.SendMessage(MakeValidSendRequest(std::string(kCrossExecutorCreateMessageId)), create_context)
           .ok()) {
    return {};
  }
  task_store.CoordinateNextTwoSnapshots();
  CrossExecutorResult result;
  const auto send = [&](a2a::examples::ExampleExecutor* executor, std::string_view message_id, std::size_t index) {
    auto request = MakeValidSendRequest(std::string(message_id));
    request.mutable_message()->set_task_id(std::string(kCrossExecutorTaskId));
    a2a::server::RequestContext context;
    result.succeeded[index] = executor->SendMessage(request, context).ok();
  };
  std::thread first(send, &first_executor, kCrossExecutorFirstMessageId, 0U);
  std::thread second(send, &second_executor, kCrossExecutorSecondMessageId, 1U);
  first.join();
  second.join();
  result.stored_task = task_store.Get(kCrossExecutorTaskId);
  result.append_calls = task_store.append_calls();
  return result;
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

TEST(ExampleSupportTest, IndependentPushReadProgressesWhileTaskStoreCallIsBlocked) {
  constexpr std::string_view kBlockedTaskId = "blocked-task";
  constexpr std::string_view kConfigId = "independent-config";
  constexpr std::chrono::seconds kProgressTimeout{1};
  BlockingGetTaskStore task_store{std::string(kBlockedTaskId)};
  lf::a2a::v1::Task blocked_task;
  blocked_task.set_id(std::string(kBlockedTaskId));
  blocked_task.set_context_id("blocked-context");
  ASSERT_TRUE(task_store.CreateOrUpdate(blocked_task).ok());
  a2a::server::InMemoryPushNotificationStore push_store;
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(kBlockedTaskId));
  config.set_id(std::string(kConfigId));
  config.set_url("https://example.test/callback");
  ASSERT_TRUE(push_store.CreateOrUpdate(config).ok());
  a2a::examples::ExampleExecutorOptions options;
  options.task_store = &task_store;
  options.push_store = &push_store;
  a2a::examples::ExampleExecutor executor(std::move(options));

  auto blocked_create = std::async(std::launch::async, [&] {
    a2a::server::RequestContext context;
    lf::a2a::v1::TaskPushNotificationConfig request;
    request.set_task_id(std::string(kBlockedTaskId));
    request.set_id("blocked-create");
    request.set_url("https://example.test/blocked");
    return executor.CreateTaskPushNotificationConfig(request, context).ok();
  });
  task_store.WaitUntilBlocked();
  auto push_read = std::async(std::launch::async, [&] {
    a2a::server::RequestContext context;
    lf::a2a::v1::GetTaskPushNotificationConfigRequest request;
    request.set_task_id(std::string(kBlockedTaskId));
    request.set_id(std::string(kConfigId));
    return executor.GetTaskPushNotificationConfig(request, context).ok();
  });

  const bool progressed = push_read.wait_for(kProgressTimeout) == std::future_status::ready;
  task_store.Release();

  EXPECT_TRUE(progressed);
  EXPECT_TRUE(push_read.get());
  EXPECT_TRUE(blocked_create.get());
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

TEST(ExampleSupportTest, SendMessagePersistsCompleteTaskWithOneUpsert) {
  CountingTaskStore task_store;
  a2a::examples::ExampleExecutorOptions options;
  options.task_store = &task_store;
  options.task_id_generator = std::make_shared<FixedTaskIdGenerator>(std::string(kSingleUpsertTaskId));
  a2a::examples::ExampleExecutor executor(std::move(options));
  a2a::server::RequestContext context;

  const auto result = executor.SendMessage(MakeValidSendRequest(std::string(kSingleUpsertMessageId)), context);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(task_store.get_calls(), kNoStoreCalls);
  EXPECT_EQ(task_store.upsert_calls(), kSingleUpsertCallCount);
  EXPECT_EQ(task_store.append_calls(), 0);
  EXPECT_EQ(task_store.last_upsert().status().state(), lf::a2a::v1::TASK_STATE_WORKING);
  EXPECT_FALSE(task_store.last_upsert().artifacts().empty());
  ASSERT_EQ(task_store.last_upsert().history_size(), kSingleHistoryEntry);
  EXPECT_EQ(task_store.last_upsert().history(0).message_id(), kSingleUpsertMessageId);
}

TEST(ExampleSupportTest, DuplicateSendUsesAppendOnlyForDedupeTelemetry) {
  CountingTaskStore task_store;
  a2a::examples::ExampleExecutorOptions options;
  options.task_store = &task_store;
  options.task_id_generator = std::make_shared<FixedTaskIdGenerator>(std::string(kDuplicateTaskId));
  a2a::examples::ExampleExecutor executor(std::move(options));
  a2a::server::RequestContext context;
  ASSERT_TRUE(executor.SendMessage(MakeValidSendRequest(std::string(kDuplicateCreateMessageId)), context).ok());
  auto duplicate = MakeValidSendRequest(std::string(kDuplicateMessageId));
  duplicate.mutable_message()->set_task_id(std::string(kDuplicateTaskId));
  ASSERT_TRUE(executor.SendMessage(duplicate, context).ok());

  const auto result = executor.SendMessage(duplicate, context);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(task_store.upsert_calls(), kDuplicateTotalUpsertCount);
  EXPECT_EQ(task_store.append_calls(), kSingleUpsertCallCount);
  ASSERT_EQ(result.value().task().history_size(), kDuplicateHistorySize);
  const auto telemetry = task_store.GetHistoryTelemetrySnapshot();
  EXPECT_EQ(telemetry.dedupe_dropped_total, 1U);
  EXPECT_EQ(telemetry.dedupe_dropped_by_message_id_and_fingerprint, 1U);
  EXPECT_EQ(telemetry.dedupe_dropped_by_fingerprint_without_message_id, 0U);
}

TEST(ExampleSupportTest, ConcurrentExecutorsRetryConflictsWithoutLockingHistoryAppend) {
  const auto result = RunCrossExecutorConflictScenario();

  EXPECT_TRUE(result.succeeded[0]);
  EXPECT_TRUE(result.succeeded[1]);
  ASSERT_TRUE(result.stored_task.ok());
  EXPECT_EQ(result.stored_task.value().history_size(), kCrossExecutorHistorySize);
  EXPECT_TRUE(TaskContainsMessage(result.stored_task.value(), kCrossExecutorFirstMessageId));
  EXPECT_TRUE(TaskContainsMessage(result.stored_task.value(), kCrossExecutorSecondMessageId));
  EXPECT_EQ(result.append_calls, 0);
}

TEST(ExampleSupportTest, GeneratedTaskCollisionFallsBackToSnapshotAndConditionalRetry) {
  CountingTaskStore task_store;
  lf::a2a::v1::Task existing;
  existing.set_id(std::string(kSingleUpsertTaskId));
  existing.set_context_id(std::string(kCollisionContextId));
  existing.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  ASSERT_TRUE(task_store.CreateOrUpdate(existing).ok());
  task_store.ResetCounts();
  a2a::examples::ExampleExecutorOptions options;
  options.task_store = &task_store;
  options.task_id_generator = std::make_shared<FixedTaskIdGenerator>(std::string(kSingleUpsertTaskId));
  a2a::examples::ExampleExecutor executor(std::move(options));
  a2a::server::RequestContext context;

  const auto result = executor.SendMessage(MakeValidSendRequest(std::string(kSingleUpsertMessageId)), context);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(task_store.get_calls(), kCollisionReadCount);
  EXPECT_EQ(task_store.upsert_calls(), kCollisionWriteCount);
  EXPECT_TRUE(TaskContainsMessage(result.value().task(), kSingleUpsertMessageId));
}

TEST(ExampleSupportTest, FollowUpValidationUsesSingleAuthoritativeSnapshot) {
  CountingTaskStore task_store;
  a2a::examples::ExampleExecutorOptions options;
  options.task_store = &task_store;
  options.task_id_generator = std::make_shared<FixedTaskIdGenerator>(std::string(kSingleUpsertTaskId));
  a2a::examples::ExampleExecutor executor(std::move(options));
  a2a::server::RequestContext context;
  ASSERT_TRUE(executor.SendMessage(MakeValidSendRequest(std::string(kSingleUpsertMessageId)), context).ok());
  task_store.ResetCounts();
  auto follow_up = MakeValidSendRequest(std::string(kSingleSnapshotFollowUpMessageId));
  follow_up.mutable_message()->set_task_id(std::string(kSingleUpsertTaskId));

  const auto result = executor.SendMessage(follow_up, context);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(task_store.get_calls(), kSingleUpsertCallCount);
  EXPECT_EQ(task_store.upsert_calls(), kSingleUpsertCallCount);
}

TEST(ExampleSupportTest, FollowUpValidationRejectsMissingAndMismatchedTasks) {
  CountingTaskStore task_store;
  a2a::examples::ExampleExecutorOptions options;
  options.task_store = &task_store;
  options.task_id_generator = std::make_shared<FixedTaskIdGenerator>(std::string(kSingleUpsertTaskId));
  a2a::examples::ExampleExecutor executor(std::move(options));
  a2a::server::RequestContext context;
  auto missing = MakeValidSendRequest(std::string(kMissingFollowUpMessageId));
  missing.mutable_message()->set_task_id(std::string(kMissingFollowUpTaskId));
  EXPECT_FALSE(executor.SendMessage(missing, context).ok());

  ASSERT_TRUE(executor.SendMessage(MakeValidSendRequest(std::string(kSingleUpsertMessageId)), context).ok());
  auto mismatch = MakeValidSendRequest(std::string(kMismatchedFollowUpMessageId));
  mismatch.mutable_message()->set_task_id(std::string(kSingleUpsertTaskId));
  mismatch.mutable_message()->set_context_id(std::string(kMismatchedFollowUpContextId));
  EXPECT_FALSE(executor.SendMessage(mismatch, context).ok());
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

TEST(ExampleSupportTest, ConcurrentSameTaskFollowUpsPreserveBothMessages) {
  constexpr std::string_view kTaskId = "same-task-concurrency";
  constexpr std::string_view kFirstMessageId = "same-task-first";
  constexpr std::string_view kSecondMessageId = "same-task-second";
  constexpr std::array<std::string_view, 2> kMessageIds = {kFirstMessageId, kSecondMessageId};
  auto executor = MakeExecutorWithTaskId(std::string(kTaskId));
  a2a::server::RequestContext create_context;
  ASSERT_TRUE(executor.SendMessage(MakeValidSendRequest("same-task-create"), create_context).ok());
  const auto succeeded = RunConcurrentFollowUps(executor, kTaskId, kMessageIds);

  EXPECT_TRUE(succeeded[0]);
  EXPECT_TRUE(succeeded[1]);
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(std::string(kTaskId));
  a2a::server::RequestContext get_context;
  const auto task = executor.GetTask(get, get_context);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().id(), kTaskId);
  EXPECT_TRUE(TaskContainsMessage(task.value(), kFirstMessageId));
  EXPECT_TRUE(TaskContainsMessage(task.value(), kSecondMessageId));
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
