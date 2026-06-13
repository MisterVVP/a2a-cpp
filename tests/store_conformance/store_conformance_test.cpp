// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <array>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "a2a/core/error.h"
#include "a2a/server/push_notification_store.h"
#include "a2a/server/server.h"
#include "store_conformance/push_notification_store_conformance.h"
#include "store_conformance/task_store_conformance.h"

#ifdef A2A_ENABLE_POSTGRES_STORE
#include "a2a/server/stores/postgres_notification_store.h"
#include "a2a/server/stores/postgres_store.h"
#include "a2a/server/stores/postgres_task_store.h"
#endif

namespace {

TEST(StoreConformanceTest, InMemoryTaskStore) {
  a2a::tests::store_conformance::RunTaskStoreConformance(
      [] { return std::make_unique<a2a::server::InMemoryTaskStore>(); });
}

TEST(StoreConformanceTest, InMemoryPushNotificationStore) {
  a2a::tests::store_conformance::RunPushNotificationStoreConformance(
      [] { return std::make_unique<a2a::server::InMemoryPushNotificationStore>(); });
}

#ifdef A2A_ENABLE_POSTGRES_STORE
[[nodiscard]] const char* GetPostgresDsn() { return std::getenv("A2A_TEST_POSTGRES_DSN"); }

[[nodiscard]] a2a::core::Error MakePostgresAcquireFailureForTesting() {
  return a2a::core::Error::Internal("test postgres acquire failure");
}

void ExpectPostgresAcquireFailure(const a2a::core::Error& error) {
  EXPECT_EQ(error.code(), a2a::core::ErrorCode::kInternal);
  EXPECT_NE(error.message().find("test postgres acquire failure"), std::string_view::npos);
}

[[nodiscard]] std::string MakePostgresTestSchema(std::string_view suffix) {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return "a2a_test_" + std::to_string(ticks) + "_" + std::string(suffix);
}

constexpr std::string_view kTargetContext = "target-context";
constexpr std::string_view kOtherContext = "other-context";
constexpr std::string_view kOldTargetTaskId = "old-target-context-task";
constexpr std::string_view kNewTargetTaskId = "new-target-context-task";
constexpr std::string_view kOtherContextTaskId = "other-context-task";
constexpr std::string_view kCompletedTargetTaskId = "completed-target-context-task";
constexpr std::size_t kFilteredTaskCount = 2;
constexpr std::size_t kSingleTaskPageSize = 1;
constexpr int kOldTargetTaskTimestampSeconds = 1000;
constexpr int kNewTargetTaskTimestampSeconds = 3000;
constexpr int kOtherContextTaskTimestampSeconds = 4000;
constexpr int kCompletedTargetTaskTimestampSeconds = 5000;

void AddPostgresTask(a2a::server::stores::PostgresTaskStore& store, std::string_view task_id,
                     std::string_view context_id, lf::a2a::v1::TaskState state, int timestamp_seconds) {
  ASSERT_TRUE(store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(std::string(task_id), std::string(context_id),
                                                                          state, timestamp_seconds))
                  .ok());
}

void SeedPostgresFilteredPaginationTasks(a2a::server::stores::PostgresTaskStore& store) {
  AddPostgresTask(store, kOldTargetTaskId, kTargetContext, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  AddPostgresTask(store, kNewTargetTaskId, kTargetContext, lf::a2a::v1::TASK_STATE_WORKING,
                  kNewTargetTaskTimestampSeconds);
  AddPostgresTask(store, kOtherContextTaskId, kOtherContext, lf::a2a::v1::TASK_STATE_WORKING,
                  kOtherContextTaskTimestampSeconds);
  AddPostgresTask(store, kCompletedTargetTaskId, kTargetContext, lf::a2a::v1::TASK_STATE_COMPLETED,
                  kCompletedTargetTaskTimestampSeconds);
}

void ExpectFilteredPostgresListPage(const a2a::server::ListTasksResponse& page, std::string_view expected_task_id,
                                    bool expect_next_page) {
  EXPECT_EQ(page.total_size, kFilteredTaskCount);
  ASSERT_EQ(page.tasks.size(), kSingleTaskPageSize);
  EXPECT_EQ(page.tasks.front().id(), expected_task_id);
  EXPECT_EQ(!page.next_page_token.empty(), expect_next_page);
}

TEST(StoreConformanceTest, PostgresStoreBundleRejectsInvalidSchemaBeforeConnecting) {
  constexpr std::string_view kInvalidConnectionString = "postgresql://invalid-host.invalid/a2a";
  constexpr std::array<std::string_view, 5> kInvalidSchemas = {"", "1invalid", "bad-name", "bad.schema", "bad;schema"};

  for (const std::string_view schema : kInvalidSchemas) {
    const a2a::server::stores::PostgresStoreFactory factory(
        {.connection_string = std::string(kInvalidConnectionString), .schema = std::string(schema)});

    const auto result = factory.CreateStoreBundle();

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  }
}

TEST(StoreConformanceTest, PostgresStoreConstructorsRejectInvalidSchemaBeforeConnecting) {
  constexpr std::string_view kInvalidConnectionString = "postgresql://invalid-host.invalid/a2a";
  constexpr std::string_view kInvalidSchema = "bad-schema";

  EXPECT_THROW((void)a2a::server::stores::PostgresTaskStore(
                   {.connection_string = std::string(kInvalidConnectionString), .schema = std::string(kInvalidSchema)}),
               std::invalid_argument);
  EXPECT_THROW((void)a2a::server::stores::PostgresPushNotificationStore(
                   {.connection_string = std::string(kInvalidConnectionString), .schema = std::string(kInvalidSchema)}),
               std::invalid_argument);
}

TEST(StoreConformanceTest, PostgresTaskStore) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("task");
  a2a::tests::store_conformance::RunTaskStoreConformance([&] {
    return std::make_unique<a2a::server::stores::PostgresTaskStore>(
        a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});
  });

  a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn, .schema = schema};
  a2a::server::stores::PostgresTaskStore first(options);
  a2a::server::stores::PostgresTaskStore second(options);
  ASSERT_TRUE(first
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask("shared-postgres-task", "shared-context",
                                                                          lf::a2a::v1::TASK_STATE_WORKING, 2000))
                  .ok());
  const auto shared = second.Get("shared-postgres-task");
  ASSERT_TRUE(shared.ok());
  EXPECT_EQ(shared.value().id(), "shared-postgres-task");
}

TEST(StoreConformanceTest, PostgresTaskStoreListAppliesFiltersBeforePagination) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("task_filtered_page");
  a2a::server::stores::PostgresTaskStore store(
      a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});
  SeedPostgresFilteredPaginationTasks(store);

  a2a::server::ListTasksRequest request;
  request.context_id = kTargetContext;
  request.status_filter = lf::a2a::v1::TASK_STATE_WORKING;
  request.page_size = kSingleTaskPageSize;

  const auto first_page = store.List(request);
  ASSERT_TRUE(first_page.ok());
  ExpectFilteredPostgresListPage(first_page.value(), kOldTargetTaskId, true);

  request.page_token = first_page.value().next_page_token;
  const auto second_page = store.List(request);
  ASSERT_TRUE(second_page.ok());
  ExpectFilteredPostgresListPage(second_page.value(), kNewTargetTaskId, false);
}

TEST(StoreConformanceTest, PostgresTaskStorePropagatesAcquireFailures) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("task_acquire_failure");
  a2a::server::stores::PostgresTaskStore store(
      a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto create = store.CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
      "lease-error-task", "lease-error-context", lf::a2a::v1::TASK_STATE_WORKING, 1000));
  ASSERT_FALSE(create.ok());
  ExpectPostgresAcquireFailure(create.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto get = store.Get("lease-error-task");
  ASSERT_FALSE(get.ok());
  ExpectPostgresAcquireFailure(get.error());

  a2a::server::ListTasksRequest list_request;
  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto list = store.List(list_request);
  ASSERT_FALSE(list.ok());
  ExpectPostgresAcquireFailure(list.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto cancel = store.Cancel("lease-error-task");
  ASSERT_FALSE(cancel.ok());
  ExpectPostgresAcquireFailure(cancel.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto append = store.AppendTaskHistory(
      "lease-error-task", a2a::tests::store_conformance::MakeMessage("lease-error-message", "message"),
      a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup);
  ASSERT_FALSE(append.ok());
  ExpectPostgresAcquireFailure(append.error());
}

TEST(StoreConformanceTest, PostgresPushNotificationStorePropagatesAcquireFailures) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("push_acquire_failure");
  a2a::server::stores::PostgresPushNotificationStore store(
      a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto create =
      store.CreateOrUpdate(a2a::tests::store_conformance::MakeConfig("lease-error-task", "lease-error-config"));
  ASSERT_FALSE(create.ok());
  ExpectPostgresAcquireFailure(create.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto get = store.Get("lease-error-task", "lease-error-config");
  ASSERT_FALSE(get.ok());
  ExpectPostgresAcquireFailure(get.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto list = store.List("lease-error-task");
  ASSERT_FALSE(list.ok());
  ExpectPostgresAcquireFailure(list.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto deleted = store.Delete("lease-error-task", "lease-error-config");
  ASSERT_FALSE(deleted.ok());
  ExpectPostgresAcquireFailure(deleted.error());
}

TEST(StoreConformanceTest, PostgresStoreFactoryPropagatesAcquireFailures) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;

  a2a::server::stores::PostgresStoreFactory task_factory(
      {.connection_string = dsn, .schema = MakePostgresTestSchema("factory_task_acquire_failure")});
  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto task_store = task_factory.CreateTaskStore();
  ASSERT_FALSE(task_store.ok());
  ExpectPostgresAcquireFailure(task_store.error());

  a2a::server::stores::PostgresStoreFactory push_factory(
      {.connection_string = dsn, .schema = MakePostgresTestSchema("factory_push_acquire_failure")});
  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto push_store = push_factory.CreatePushNotificationStore();
  ASSERT_FALSE(push_store.ok());
  ExpectPostgresAcquireFailure(push_store.error());

  a2a::server::stores::PostgresStoreFactory bundle_factory(
      {.connection_string = dsn, .schema = MakePostgresTestSchema("factory_bundle_acquire_failure")});
  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto bundle = bundle_factory.CreateStoreBundle();
  ASSERT_FALSE(bundle.ok());
  ExpectPostgresAcquireFailure(bundle.error());
}

TEST(StoreConformanceTest, PostgresPushNotificationStore) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("push");
  a2a::tests::store_conformance::RunPushNotificationStoreConformance([&] {
    return std::make_unique<a2a::server::stores::PostgresPushNotificationStore>(
        a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});
  });

  a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn, .schema = schema};
  a2a::server::stores::PostgresPushNotificationStore first(options);
  a2a::server::stores::PostgresPushNotificationStore second(options);
  ASSERT_TRUE(
      first.CreateOrUpdate(a2a::tests::store_conformance::MakeConfig("shared-postgres-task", "shared-postgres-config"))
          .ok());
  const auto shared = second.Get("shared-postgres-task", "shared-postgres-config");
  ASSERT_TRUE(shared.ok());
  EXPECT_EQ(shared.value().id(), "shared-postgres-config");
}
#endif

}  // namespace
