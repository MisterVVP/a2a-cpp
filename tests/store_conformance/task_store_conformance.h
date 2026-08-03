// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::tests::store_conformance {

constexpr std::string_view kTaskOne = "conformance-task-1";
constexpr std::string_view kTaskTwo = "conformance-task-2";
constexpr std::string_view kTaskWithoutStatusTimestamp = "conformance-task-without-status-timestamp";
constexpr std::string_view kContextOne = "conformance-context-1";
constexpr std::string_view kContextTwo = "conformance-context-2";
constexpr std::string_view kContextWithoutStatusTimestamp = "conformance-context-without-status-timestamp";
constexpr int64_t kEpochSeconds = 0;
constexpr int32_t kEpochNanos = 0;
constexpr int64_t kBaseSeconds = 1000;
constexpr int32_t kBaseNanos = 7;
constexpr std::string_view kRevisionTaskId = "conformance-revision-task";
constexpr std::string_view kRevisionCancelTaskId = "conformance-revision-cancel-task";
constexpr std::string_view kRevisionAppendTaskId = "conformance-revision-append-task";
constexpr std::string_view kRevisionContext = "conformance-revision-context";
constexpr std::string_view kRevisionMessageId = "conformance-revision-message";
constexpr std::string_view kRevisionMessageText = "revision message";
constexpr std::uint64_t kMinimumRevision = 1;

[[nodiscard]] inline lf::a2a::v1::Message MakeMessage(std::string id, std::string text) {
  lf::a2a::v1::Message message;
  message.set_message_id(std::move(id));
  message.set_role(lf::a2a::v1::ROLE_USER);
  message.add_parts()->set_text(std::move(text));
  return message;
}

[[nodiscard]] inline lf::a2a::v1::Task MakeTask(std::string id, std::string context, lf::a2a::v1::TaskState state,
                                                int64_t seconds) {
  lf::a2a::v1::Task task;
  task.set_id(std::move(id));
  task.set_context_id(std::move(context));
  task.mutable_status()->set_state(state);
  task.mutable_status()->mutable_timestamp()->set_seconds(seconds);
  task.mutable_status()->mutable_timestamp()->set_nanos(kBaseNanos);
  task.add_artifacts()->set_artifact_id("artifact");
  *task.add_history() = MakeMessage("history-1", "first");
  *task.add_history() = MakeMessage("history-2", "second");
  return task;
}

[[nodiscard]] inline lf::a2a::v1::Task MakeTaskWithoutStatusTimestamp(std::string id, std::string context,
                                                                      lf::a2a::v1::TaskState state) {
  lf::a2a::v1::Task task;
  task.set_id(std::move(id));
  task.set_context_id(std::move(context));
  task.mutable_status()->set_state(state);
  return task;
}

inline void VerifyOptimisticPersistenceContract(a2a::server::TaskStore* store) {
  auto task = MakeTask(std::string(kRevisionTaskId), std::string(kRevisionContext), lf::a2a::v1::TASK_STATE_WORKING,
                       kBaseSeconds);
  ASSERT_TRUE(store->CreateOrUpdate(task).ok());
  const auto initial = store->GetSnapshot(kRevisionTaskId);
  ASSERT_TRUE(initial.ok());
  EXPECT_GE(initial.value().revision, kMinimumRevision);
  task.set_context_id(std::string(kContextTwo));
  const auto updated = store->CreateOrUpdateIfRevision(task, initial.value().revision);
  ASSERT_TRUE(updated.ok());
  EXPECT_EQ(updated.value(), a2a::server::TaskStore::ConditionalWriteResult::kUpdated);
  const auto advanced = store->GetSnapshot(kRevisionTaskId);
  ASSERT_TRUE(advanced.ok());
  EXPECT_GT(advanced.value().revision, initial.value().revision);
  auto stale_task = task;
  stale_task.set_context_id(std::string(kContextOne));
  const auto stale = store->CreateOrUpdateIfRevision(stale_task, initial.value().revision);
  ASSERT_TRUE(stale.ok());
  EXPECT_EQ(stale.value(), a2a::server::TaskStore::ConditionalWriteResult::kConflict);
  const auto after_conflict = store->Get(kRevisionTaskId);
  ASSERT_TRUE(after_conflict.ok());
  EXPECT_EQ(after_conflict.value().context_id(), kContextTwo);

  ASSERT_TRUE(store
                  ->CreateOrUpdate(MakeTask(std::string(kRevisionCancelTaskId), std::string(kRevisionContext),
                                            lf::a2a::v1::TASK_STATE_WORKING, kBaseSeconds))
                  .ok());
  const auto before_cancel = store->GetSnapshot(kRevisionCancelTaskId);
  ASSERT_TRUE(before_cancel.ok());
  ASSERT_TRUE(store->Cancel(kRevisionCancelTaskId).ok());
  const auto after_cancel = store->GetSnapshot(kRevisionCancelTaskId);
  ASSERT_TRUE(after_cancel.ok());
  EXPECT_GT(after_cancel.value().revision, before_cancel.value().revision);

  auto append_task = MakeTask(std::string(kRevisionAppendTaskId), std::string(kRevisionContext),
                              lf::a2a::v1::TASK_STATE_WORKING, kBaseSeconds);
  append_task.clear_history();
  ASSERT_TRUE(store->CreateOrUpdate(append_task).ok());
  const auto before_append = store->GetSnapshot(kRevisionAppendTaskId);
  ASSERT_TRUE(before_append.ok());
  ASSERT_TRUE(store
                  ->AppendTaskHistory(kRevisionAppendTaskId,
                                      MakeMessage(std::string(kRevisionMessageId), std::string(kRevisionMessageText)),
                                      a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup)
                  .ok());
  const auto after_append = store->GetSnapshot(kRevisionAppendTaskId);
  ASSERT_TRUE(after_append.ok());
  EXPECT_GT(after_append.value().revision, before_append.value().revision);
}

template <typename Factory>
void RunTaskStoreConformance(Factory&& factory) {
  auto store = factory();
  ASSERT_TRUE(store->SupportsConditionalWrites());
  ASSERT_TRUE(store
                  ->CreateOrUpdate(MakeTask(std::string(kTaskOne), std::string(kContextOne),
                                            lf::a2a::v1::TASK_STATE_WORKING, kBaseSeconds))
                  .ok());
  ASSERT_TRUE(store
                  ->CreateOrUpdate(MakeTask(std::string(kTaskTwo), std::string(kContextTwo),
                                            lf::a2a::v1::TASK_STATE_COMPLETED, kBaseSeconds + 1))
                  .ok());

  const auto fetched = store->Get(kTaskOne);
  ASSERT_TRUE(fetched.ok());
  EXPECT_EQ(fetched.value().id(), kTaskOne);
  EXPECT_FALSE(store->Get("missing-task").ok());

  a2a::server::ListTasksRequest page_request;
  page_request.page_size = 1;
  const auto first_page = store->List(page_request);
  ASSERT_TRUE(first_page.ok());
  EXPECT_EQ(first_page.value().page_size, 1U);
  ASSERT_EQ(first_page.value().tasks.size(), 1U);
  EXPECT_EQ(first_page.value().tasks.front().id(), kTaskOne);
  EXPECT_FALSE(first_page.value().next_page_token.empty());

  a2a::server::ListTasksRequest context_request;
  context_request.context_id = std::string(kContextOne);
  const auto context_list = store->List(context_request);
  ASSERT_TRUE(context_list.ok());
  ASSERT_EQ(context_list.value().tasks.size(), 1U);
  EXPECT_EQ(context_list.value().tasks.front().context_id(), kContextOne);

  a2a::server::ListTasksRequest status_request;
  status_request.status_filter = lf::a2a::v1::TASK_STATE_COMPLETED;
  const auto status_list = store->List(status_request);
  ASSERT_TRUE(status_list.ok());
  ASSERT_EQ(status_list.value().tasks.size(), 1U);
  EXPECT_EQ(status_list.value().tasks.front().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);

  a2a::server::ListTasksRequest timestamp_request;
  timestamp_request.status_timestamp_after.emplace();
  timestamp_request.status_timestamp_after->set_seconds(kBaseSeconds + 1);
  const auto timestamp_list = store->List(timestamp_request);
  ASSERT_TRUE(timestamp_list.ok());
  ASSERT_FALSE(timestamp_list.value().tasks.empty());
  for (const auto& task : timestamp_list.value().tasks) {
    EXPECT_GE(task.status().timestamp().seconds(), kBaseSeconds + 1);
  }

  ASSERT_TRUE(store
                  ->CreateOrUpdate(MakeTaskWithoutStatusTimestamp(std::string(kTaskWithoutStatusTimestamp),
                                                                  std::string(kContextWithoutStatusTimestamp),
                                                                  lf::a2a::v1::TASK_STATE_WORKING))
                  .ok());
  const auto without_timestamp = store->Get(kTaskWithoutStatusTimestamp);
  ASSERT_TRUE(without_timestamp.ok());
  ASSERT_FALSE(without_timestamp.value().status().has_timestamp());

  a2a::server::ListTasksRequest epoch_timestamp_request;
  epoch_timestamp_request.status_timestamp_after.emplace();
  epoch_timestamp_request.status_timestamp_after->set_seconds(kEpochSeconds);
  epoch_timestamp_request.status_timestamp_after->set_nanos(kEpochNanos);
  const auto epoch_timestamp_list = store->List(epoch_timestamp_request);
  ASSERT_TRUE(epoch_timestamp_list.ok());
  for (const auto& task : epoch_timestamp_list.value().tasks) {
    EXPECT_NE(task.id(), kTaskWithoutStatusTimestamp);
    EXPECT_TRUE(task.status().has_timestamp());
  }

  a2a::server::ListTasksRequest projection_request;
  projection_request.history_length = 1;
  projection_request.include_artifacts = false;
  const auto projected = store->List(projection_request);
  ASSERT_TRUE(projected.ok());
  ASSERT_FALSE(projected.value().tasks.empty());
  EXPECT_EQ(projected.value().tasks.front().artifacts_size(), 0);
  EXPECT_LE(projected.value().tasks.front().history_size(), 1);

  const auto canceled = store->Cancel(kTaskOne);
  ASSERT_TRUE(canceled.ok());
  EXPECT_EQ(canceled.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
  EXPECT_FALSE(store->Cancel(kTaskTwo).ok());

  auto append_task =
      MakeTask("append-task", std::string(kContextOne), lf::a2a::v1::TASK_STATE_WORKING, kBaseSeconds + 2);
  append_task.clear_history();
  ASSERT_TRUE(store->CreateOrUpdate(append_task).ok());
  const auto appended = store->AppendTaskHistory("append-task", MakeMessage("message-1", "same"),
                                                 a2a::server::TaskStore::HistoryAppendPolicy::kDedupByMessageId);
  ASSERT_TRUE(appended.ok());
  EXPECT_EQ(appended.value().history_size(), 1);
  const auto deduped = store->AppendTaskHistory("append-task", MakeMessage("message-1", "same"),
                                                a2a::server::TaskStore::HistoryAppendPolicy::kDedupByMessageId);
  ASSERT_TRUE(deduped.ok());
  EXPECT_EQ(deduped.value().history_size(), 1);
  EXPECT_EQ(store->GetHistoryTelemetrySnapshot().dedupe_dropped_total, 1U);
  VerifyOptimisticPersistenceContract(store.get());
}

}  // namespace a2a::tests::store_conformance
