#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <unordered_map>

#include "a2a/server/server.h"

namespace {

constexpr std::string_view kContextAlpha = "context-alpha";
constexpr std::string_view kContextBeta = "context-beta";
constexpr std::size_t kDefaultPageSize = 0;
constexpr std::size_t kFirstPageSize = 1;
constexpr std::size_t kTwoHistoryEntries = 2;
constexpr int64_t kTimestampBaseSeconds = 100;
constexpr int32_t kTimestampNanos = 1;

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

}  // namespace
