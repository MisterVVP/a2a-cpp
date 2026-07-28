// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <google/protobuf/unknown_field_set.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "a2a/server/request_context.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_history.h"
#include "a2a/server/tasks/task_lifecycle_service.h"
#include "a2a/server/tasks/task_ordering.h"
#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"

namespace {

constexpr std::string_view kContextAlpha = "context-alpha";
constexpr std::string_view kContextBeta = "context-beta";
constexpr std::size_t kDefaultPageSize = 0;
constexpr std::size_t kFirstPageSize = 1;
constexpr std::size_t kTwoHistoryEntries = 2;
constexpr std::size_t kThreeHistoryEntries = 3;
constexpr int64_t kTimestampBaseSeconds = 100;
constexpr int32_t kTimestampNanos = 1;
constexpr int64_t kOrderOlderSeconds = 10;
constexpr int64_t kOrderNewestSeconds = 11;
constexpr int32_t kOrderOlderNanos = 2;
constexpr int32_t kOrderNewestNanos = 1;
constexpr int32_t kOrderTieHigherNanos = 9;
constexpr std::string_view kTaskId = "task-append";
constexpr std::string_view kProjectionTaskId = "task-projection";
constexpr std::string_view kFirstTaskId = "task-1";
constexpr std::string_view kSecondTaskId = "task-2";
constexpr std::string_view kThirdTaskId = "task-3";
constexpr std::string_view kLastHistoryMessageId = "message-2";
constexpr std::string_view kFirstOffsetToken = "1";
constexpr std::string_view kSecondOffsetToken = "2";
constexpr int kUnknownTaskFieldNumber = 1000;
constexpr std::uint64_t kUnknownTaskFieldValue = 42;

class RecordingHistoryTelemetrySink final : public a2a::server::InMemoryTaskStore::HistoryTelemetrySink {
 public:
  void OnDedupedHistoryMessage(const a2a::server::TaskStore::HistoryDedupeEvent& event) override {
    events.push_back(event);
  }

  std::vector<a2a::server::TaskStore::HistoryDedupeEvent> events;
};

lf::a2a::v1::Task MakeTask(std::string id, std::string context_id, lf::a2a::v1::TaskState state,
                           int64_t timestamp_seconds, bool include_artifact,
                           std::size_t history_entries = kTwoHistoryEntries) {
  lf::a2a::v1::Task task;
  task.set_id(std::move(id));
  task.set_context_id(std::move(context_id));
  task.mutable_status()->set_state(state);
  task.mutable_status()->mutable_timestamp()->set_seconds(timestamp_seconds);
  task.mutable_status()->mutable_timestamp()->set_nanos(kTimestampNanos);

  if (include_artifact) {
    task.add_artifacts()->set_artifact_id("artifact-1");
  }

  for (std::size_t index = 0; index < history_entries; ++index) {
    auto* history_message = task.add_history();
    history_message->set_message_id("message-" + std::to_string(index));
  }

  return task;
}

TEST(ServerHelpersTest, ExtractAuthMetadataNormalizesAndCollectsSignals) {
  const std::unordered_map<std::string, std::string> headers = {{"Authorization", "  Bearer sample-token  "},
                                                                {"X-API-Key", "key-123"},
                                                                {"X-Forwarded-Client-Cert", "cert-chain"},
                                                                {"X-Custom-Token", "custom-token"},
                                                                {"X-Auth-Provider", "provider"}};

  const auto metadata = a2a::server::ExtractAuthMetadata(headers);

  ASSERT_EQ(metadata.at("authorization"), "Bearer sample-token");
  ASSERT_EQ(metadata.at("api_key"), "key-123");
  ASSERT_EQ(metadata.at("mtls_client_cert"), "cert-chain");
  ASSERT_EQ(metadata.at("header.authorization"), "  Bearer sample-token  ");
  ASSERT_EQ(metadata.at("header.x-custom-token"), "custom-token");
  ASSERT_EQ(metadata.at("header.x-auth-provider"), "provider");
  ASSERT_EQ(metadata.at("bearer_token"), "sample-token");
}

TEST(InMemoryTaskStoreUnitTest, ValidatesInputAndReturnsNotFoundErrors) {
  a2a::server::InMemoryTaskStore store;

  lf::a2a::v1::Task invalid_task;
  const auto create_result = store.CreateOrUpdate(invalid_task);
  ASSERT_FALSE(create_result.ok());

  const auto missing_get_result = store.Get("");
  ASSERT_FALSE(missing_get_result.ok());

  const auto missing_cancel_result = store.Cancel("missing-id");
  ASSERT_FALSE(missing_cancel_result.ok());

  a2a::server::ListTasksRequest invalid_list_request;
  invalid_list_request.page_token = "not-a-number";
  const auto invalid_list_result = store.List(invalid_list_request);
  ASSERT_FALSE(invalid_list_result.ok());
}

TEST(InMemoryTaskStoreUnitTest, AppliesFilteringPaginationAndProjectionOptions) {
  a2a::server::InMemoryTaskStore store;

  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask("task-1", std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_WORKING,
                                           kTimestampBaseSeconds, true))
                  .ok());
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask("task-2", std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_CANCELED,
                                           kTimestampBaseSeconds + 1, false, 1))
                  .ok());
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask("task-3", std::string(kContextBeta), lf::a2a::v1::TASK_STATE_WORKING,
                                           kTimestampBaseSeconds + 2, true))
                  .ok());

  a2a::server::ListTasksRequest first_page_request(kFirstPageSize, "0");
  first_page_request.context_id = std::string(kContextAlpha);
  first_page_request.status_filter = lf::a2a::v1::TASK_STATE_WORKING;
  google::protobuf::Timestamp cutoff;
  cutoff.set_seconds(kTimestampBaseSeconds);
  cutoff.set_nanos(kTimestampNanos);
  first_page_request.status_timestamp_after = cutoff;
  first_page_request.include_artifacts = false;
  first_page_request.history_length = std::size_t{1};

  const auto first_page_result = store.List(first_page_request);
  ASSERT_TRUE(first_page_result.ok());
  const auto& first_page = first_page_result.value();
  ASSERT_EQ(first_page.total_size, 1U);
  ASSERT_EQ(first_page.page_size, 1U);
  ASSERT_TRUE(first_page.next_page_token.empty());
  ASSERT_EQ(first_page.tasks.size(), 1U);
  EXPECT_EQ(first_page.tasks.front().id(), "task-1");
  EXPECT_EQ(first_page.tasks.front().history_size(), 1);
  EXPECT_EQ(first_page.tasks.front().artifacts_size(), 0);

  a2a::server::ListTasksRequest second_page_request(kFirstPageSize, "1");
  const auto second_page_result = store.List(second_page_request);
  ASSERT_TRUE(second_page_result.ok());
  const auto& second_page = second_page_result.value();
  EXPECT_EQ(second_page.tasks.size(), 1U);
  EXPECT_EQ(second_page.tasks.front().id(), "task-2");

  a2a::server::ListTasksRequest all_tasks_request(kDefaultPageSize, "");
  all_tasks_request.history_length = std::size_t{0};
  all_tasks_request.include_artifacts = true;
  const auto all_tasks_result = store.List(all_tasks_request);
  ASSERT_TRUE(all_tasks_result.ok());
  ASSERT_EQ(all_tasks_result.value().tasks.size(), 3U);
  EXPECT_EQ(all_tasks_result.value().tasks.front().history_size(), 0);
}

TEST(InMemoryTaskStoreUnitTest, CancelUpdatesStateAndRejectsTerminalTasks) {
  a2a::server::InMemoryTaskStore store;

  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask("task-1", std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_WORKING,
                                           kTimestampBaseSeconds, true))
                  .ok());

  const auto canceled_result = store.Cancel("task-1");
  ASSERT_TRUE(canceled_result.ok());
  EXPECT_EQ(canceled_result.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);

  const auto second_cancel_result = store.Cancel("task-1");
  ASSERT_FALSE(second_cancel_result.ok());
}

TEST(InMemoryTaskStoreUnitTest, UpdatingTaskPreservesItsListPosition) {
  a2a::server::InMemoryTaskStore store;
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kFirstTaskId), std::string(kContextAlpha),
                                           lf::a2a::v1::TASK_STATE_WORKING, kTimestampBaseSeconds, false))
                  .ok());
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kSecondTaskId), std::string(kContextBeta),
                                           lf::a2a::v1::TASK_STATE_WORKING, kTimestampBaseSeconds, false))
                  .ok());
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kFirstTaskId), std::string(kContextBeta),
                                           lf::a2a::v1::TASK_STATE_COMPLETED, kTimestampBaseSeconds, false))
                  .ok());

  const auto result = store.List(a2a::server::ListTasksRequest{});

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.value().tasks.size(), 2U);
  EXPECT_EQ(result.value().tasks.front().id(), kFirstTaskId);
  EXPECT_EQ(result.value().tasks.front().context_id(), kContextBeta);
  EXPECT_EQ(result.value().tasks.front().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  EXPECT_EQ(result.value().tasks.back().id(), kSecondTaskId);
}

TEST(InMemoryTaskStoreUnitTest, ProjectsArtifactsAndRequestedHistoryWithoutMutatingStoredTask) {
  a2a::server::InMemoryTaskStore store;
  auto source = MakeTask(std::string(kProjectionTaskId), std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_WORKING,
                         kTimestampBaseSeconds, true, kThreeHistoryEntries);
  const auto* source_reflection = source.GetReflection();
  source_reflection->MutableUnknownFields(&source)->AddVarint(kUnknownTaskFieldNumber, kUnknownTaskFieldValue);
  ASSERT_TRUE(store.CreateOrUpdate(source).ok());
  const std::string serialized_source = source.SerializeAsString();

  a2a::server::ListTasksRequest no_history_request;
  no_history_request.history_length = std::size_t{0};
  const auto no_history = store.List(no_history_request);
  ASSERT_TRUE(no_history.ok());
  ASSERT_EQ(no_history.value().tasks.size(), 1U);
  EXPECT_EQ(no_history.value().tasks.front().artifacts_size(), 0);
  EXPECT_EQ(no_history.value().tasks.front().history_size(), 0);
  const auto& projected_task = no_history.value().tasks.front();
  const auto* projected_reflection = projected_task.GetReflection();
  const auto& projected_unknown_fields = projected_reflection->GetUnknownFields(projected_task);
  ASSERT_EQ(projected_unknown_fields.field_count(), 1);
  EXPECT_EQ(projected_unknown_fields.field(0).number(), kUnknownTaskFieldNumber);
  EXPECT_EQ(projected_unknown_fields.field(0).type(), google::protobuf::UnknownField::TYPE_VARINT);
  EXPECT_EQ(projected_unknown_fields.field(0).varint(), kUnknownTaskFieldValue);

  a2a::server::ListTasksRequest one_history_request;
  one_history_request.history_length = std::size_t{1};
  const auto one_history = store.List(one_history_request);
  ASSERT_TRUE(one_history.ok());
  ASSERT_EQ(one_history.value().tasks.size(), 1U);
  EXPECT_EQ(one_history.value().tasks.front().history_size(), 1);
  EXPECT_EQ(one_history.value().tasks.front().history(0).message_id(), kLastHistoryMessageId);

  a2a::server::ListTasksRequest all_history_request;
  all_history_request.include_artifacts = true;
  const auto all_history = store.List(all_history_request);
  ASSERT_TRUE(all_history.ok());
  ASSERT_EQ(all_history.value().tasks.size(), 1U);
  EXPECT_EQ(all_history.value().tasks.front().artifacts_size(), 1);
  EXPECT_EQ(all_history.value().tasks.front().history_size(), static_cast<int>(kThreeHistoryEntries));

  const auto stored = store.Get(kProjectionTaskId);
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored.value().SerializeAsString(), serialized_source);
}

TEST(InMemoryTaskStoreUnitTest, PaginatesFilteredAndUnfilteredResultsInInsertionOrder) {
  a2a::server::InMemoryTaskStore store;
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kFirstTaskId), std::string(kContextAlpha),
                                           lf::a2a::v1::TASK_STATE_WORKING, kTimestampBaseSeconds, false))
                  .ok());
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kSecondTaskId), std::string(kContextBeta),
                                           lf::a2a::v1::TASK_STATE_WORKING, kTimestampBaseSeconds, false))
                  .ok());
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kThirdTaskId), std::string(kContextAlpha),
                                           lf::a2a::v1::TASK_STATE_WORKING, kTimestampBaseSeconds, false))
                  .ok());

  const auto unfiltered = store.List(a2a::server::ListTasksRequest{kFirstPageSize, std::string(kFirstOffsetToken)});
  ASSERT_TRUE(unfiltered.ok());
  ASSERT_EQ(unfiltered.value().tasks.size(), 1U);
  EXPECT_EQ(unfiltered.value().tasks.front().id(), kSecondTaskId);
  EXPECT_EQ(unfiltered.value().next_page_token, kSecondOffsetToken);

  a2a::server::ListTasksRequest filtered_request{kFirstPageSize, std::string(kFirstOffsetToken)};
  filtered_request.context_id = std::string(kContextAlpha);
  const auto filtered = store.List(filtered_request);
  ASSERT_TRUE(filtered.ok());
  ASSERT_EQ(filtered.value().tasks.size(), 1U);
  EXPECT_EQ(filtered.value().tasks.front().id(), kThirdTaskId);
  EXPECT_TRUE(filtered.value().next_page_token.empty());
  EXPECT_EQ(filtered.value().total_size, 2U);
}

TEST(InMemoryTaskStoreUnitTest, AppendTaskHistoryAppliesDedupPoliciesAndPreservesOrder) {
  a2a::server::InMemoryTaskStore store;
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kTaskId), std::string(kContextAlpha),
                                           lf::a2a::v1::TASK_STATE_WORKING, kTimestampBaseSeconds, true, 0))
                  .ok());

  lf::a2a::v1::Message first;
  first.set_message_id("m-1");
  first.set_task_id(std::string(kTaskId));
  first.add_parts()->set_text("hello");
  ASSERT_TRUE(store.AppendTaskHistory(kTaskId, first, a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup).ok());
  ASSERT_TRUE(store.AppendTaskHistory(kTaskId, first, a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup).ok());

  auto task = store.Get(kTaskId);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().history_size(), 2);
  EXPECT_EQ(task.value().history(0).message_id(), "m-1");
  EXPECT_EQ(task.value().history(1).message_id(), "m-1");

  ASSERT_TRUE(
      store.AppendTaskHistory(kTaskId, first, a2a::server::TaskStore::HistoryAppendPolicy::kDedupByMessageId).ok());
  task = store.Get(kTaskId);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().history_size(), 2);

  lf::a2a::v1::Message same_id_different_body = first;
  same_id_different_body.mutable_parts(0)->set_text("hello-updated");
  ASSERT_TRUE(store
                  .AppendTaskHistory(kTaskId, same_id_different_body,
                                     a2a::server::TaskStore::HistoryAppendPolicy::kDedupByMessageId)
                  .ok());
  task = store.Get(kTaskId);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().history_size(), 3);
  EXPECT_EQ(task.value().history(2).parts(0).text(), "hello-updated");

  lf::a2a::v1::Message no_id;
  no_id.set_task_id(std::string(kTaskId));
  no_id.set_role(lf::a2a::v1::ROLE_USER);
  no_id.add_parts()->set_text("same-without-id");
  ASSERT_TRUE(
      store.AppendTaskHistory(kTaskId, no_id, a2a::server::TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint)
          .ok());
  ASSERT_TRUE(
      store.AppendTaskHistory(kTaskId, no_id, a2a::server::TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint)
          .ok());
  task = store.Get(kTaskId);
  ASSERT_TRUE(task.ok());
  EXPECT_EQ(task.value().history_size(), 4);
  EXPECT_EQ(task.value().history(3).parts(0).text(), "same-without-id");
}

TEST(InMemoryTaskStoreUnitTest, EmitsStructuredTelemetryForDedupedRetries) {
  auto sink = std::make_shared<RecordingHistoryTelemetrySink>();
  a2a::server::InMemoryTaskStore store(sink);
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kTaskId), std::string(kContextAlpha),
                                           lf::a2a::v1::TASK_STATE_WORKING, kTimestampBaseSeconds, true, 0))
                  .ok());

  lf::a2a::v1::Message message;
  message.set_message_id("telemetry-1");
  message.set_task_id(std::string(kTaskId));
  message.add_parts()->set_text("payload");

  ASSERT_TRUE(
      store.AppendTaskHistory(kTaskId, message, a2a::server::TaskStore::HistoryAppendPolicy::kDedupByMessageId).ok());
  ASSERT_TRUE(
      store.AppendTaskHistory(kTaskId, message, a2a::server::TaskStore::HistoryAppendPolicy::kDedupByMessageId).ok());

  const auto snapshot = store.GetHistoryTelemetrySnapshot();
  EXPECT_EQ(snapshot.dedupe_dropped_total, 1U);
  EXPECT_EQ(snapshot.dedupe_dropped_by_message_id_and_fingerprint, 1U);
  EXPECT_EQ(snapshot.dedupe_dropped_by_fingerprint_without_message_id, 0U);
  ASSERT_EQ(sink->events.size(), 1U);
  EXPECT_EQ(sink->events.front().task_id, kTaskId);
  EXPECT_EQ(sink->events.front().message_id, "telemetry-1");
}

TEST(InMemoryTaskStoreUnitTest, HandlesOutOfOrderAndMixedReplayScenarios) {
  a2a::server::InMemoryTaskStore store;
  constexpr std::string_view kReplayTaskId = "task-replay";
  ASSERT_TRUE(store
                  .CreateOrUpdate(MakeTask(std::string(kReplayTaskId), std::string(kContextAlpha),
                                           lf::a2a::v1::TASK_STATE_WORKING, kTimestampBaseSeconds, true, 0))
                  .ok());

  lf::a2a::v1::Message first;
  first.set_message_id("m-1");
  first.set_task_id(std::string(kReplayTaskId));
  first.add_parts()->set_text("first");
  lf::a2a::v1::Message second;
  second.set_message_id("m-2");
  second.set_task_id(std::string(kReplayTaskId));
  second.add_parts()->set_text("second");

  ASSERT_TRUE(
      store
          .AppendTaskHistory(kReplayTaskId, first, a2a::server::TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint)
          .ok());
  ASSERT_TRUE(store
                  .AppendTaskHistory(kReplayTaskId, second,
                                     a2a::server::TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint)
                  .ok());
  ASSERT_TRUE(
      store
          .AppendTaskHistory(kReplayTaskId, first, a2a::server::TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint)
          .ok());

  lf::a2a::v1::Message first_without_id = first;
  first_without_id.clear_message_id();
  ASSERT_TRUE(
      store.AppendTaskHistory(kReplayTaskId, first_without_id, a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup)
          .ok());
  ASSERT_TRUE(store
                  .AppendTaskHistory(kReplayTaskId, first_without_id,
                                     a2a::server::TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint)
                  .ok());

  const auto result = store.Get(kReplayTaskId);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.value().history_size(), 3);
  EXPECT_EQ(result.value().history(0).message_id(), "m-1");
  EXPECT_EQ(result.value().history(1).message_id(), "m-2");
  EXPECT_EQ(result.value().history(2).message_id(), "");
}

TEST(ServerTaskUtilitiesTest, ValidatesPageTokensAndOffsets) {
  const auto empty = a2a::server::ParseListPageToken("");
  ASSERT_TRUE(empty.ok());
  EXPECT_EQ(empty.value(), 0U);

  const auto valid = a2a::server::ParseListPageToken("12");
  ASSERT_TRUE(valid.ok());
  EXPECT_EQ(valid.value(), 12U);

  const auto invalid = a2a::server::ParseListPageToken("12x");
  ASSERT_FALSE(invalid.ok());

  EXPECT_TRUE(a2a::server::ValidateListPageOffset(2, 2).ok());
  EXPECT_FALSE(a2a::server::ValidateListPageOffset(3, 2).ok());
}

TEST(ServerTaskUtilitiesTest, LifecycleServiceEnforcesTerminalStateGuard) {
  a2a::server::InMemoryTaskStore store;
  a2a::server::TaskLifecycleService lifecycle(&store);

  auto task = MakeTask("terminal-task", std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_WORKING, kOrderOlderSeconds,
                       false, 0);
  ASSERT_TRUE(lifecycle.CreateOrUpdateTask(task).ok());
  ASSERT_TRUE(lifecycle.TransitionTaskStatus("terminal-task", lf::a2a::v1::TASK_STATE_COMPLETED).ok());
  const auto rejected = lifecycle.TransitionTaskStatus("terminal-task", lf::a2a::v1::TASK_STATE_CANCELED);
  ASSERT_FALSE(rejected.ok());
}

TEST(ServerTaskUtilitiesTest, AppliesHistoryAndArtifactProjectionHelpers) {
  lf::a2a::v1::Task task = MakeTask("projection-task", std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_WORKING,
                                    kTimestampBaseSeconds, true, 3);
  a2a::server::ApplyArtifactProjection(&task, false);
  EXPECT_EQ(task.artifacts_size(), 0);

  a2a::server::ApplyHistoryRetention(&task, std::size_t{2});
  EXPECT_EQ(task.history_size(), 2);
  EXPECT_EQ(task.history(0).message_id(), "message-1");
  EXPECT_EQ(task.history(1).message_id(), "message-2");
}

TEST(ServerTaskUtilitiesTest, OrdersTasksByTimestampWithNanosTiebreaker) {
  auto older =
      MakeTask("task-older", std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_WORKING, kOrderOlderSeconds, false, 0);
  older.mutable_status()->mutable_timestamp()->set_nanos(kOrderOlderNanos);
  auto newest =
      MakeTask("task-new", std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_WORKING, kOrderNewestSeconds, false, 0);
  newest.mutable_status()->mutable_timestamp()->set_nanos(kOrderNewestNanos);
  auto tie_higher_nanos = MakeTask("task-tie-high", std::string(kContextAlpha), lf::a2a::v1::TASK_STATE_WORKING,
                                   kOrderOlderSeconds, false, 0);
  tie_higher_nanos.mutable_status()->mutable_timestamp()->set_nanos(kOrderTieHigherNanos);

  std::vector<const lf::a2a::v1::Task*> ordered = {&older, &newest, &tie_higher_nanos};
  a2a::server::TimestampDescTaskOrdering::Sort(&ordered);
  ASSERT_EQ(ordered.size(), 3U);
  EXPECT_EQ(ordered[0]->id(), "task-new");
  EXPECT_EQ(ordered[1]->id(), "task-tie-high");
  EXPECT_EQ(ordered[2]->id(), "task-older");
}

}  // namespace
