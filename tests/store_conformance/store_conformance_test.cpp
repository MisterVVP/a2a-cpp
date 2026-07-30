// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/server/push_notification_store.h"
#include "a2a/server/stores/store_factory.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_store.h"
#include "store_conformance/push_notification_store_conformance.h"
#include "store_conformance/task_store_conformance.h"

#ifdef A2A_ENABLE_POSTGRES_STORE
#include "a2a/server/stores/postgres_common.h"
#include "a2a/server/stores/postgres_notification_store.h"
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
constexpr std::size_t kConcurrentPageSize = 1;
constexpr std::array<std::string_view, 4> kConcurrentIds = {"connection-a-first", "connection-b-first",
                                                            "connection-a-second", "connection-b-second"};

template <typename Insert>
[[nodiscard]] bool RunDeterministicallyInterleavedInserts(Insert insert) {
  std::barrier synchronization_point(2);
  std::atomic<bool> succeeded{true};
  const auto insert_and_record = [&](std::size_t connection, std::string_view id) {
    if (!insert(connection, id)) {
      succeeded.store(false, std::memory_order_relaxed);
    }
  };
  std::thread connection_a([&] {
    insert_and_record(0, kConcurrentIds[0]);
    synchronization_point.arrive_and_wait();
    synchronization_point.arrive_and_wait();
    insert_and_record(0, kConcurrentIds[2]);
    synchronization_point.arrive_and_wait();
  });
  std::thread connection_b([&] {
    synchronization_point.arrive_and_wait();
    insert_and_record(1, kConcurrentIds[1]);
    synchronization_point.arrive_and_wait();
    synchronization_point.arrive_and_wait();
    insert_and_record(1, kConcurrentIds[3]);
  });
  connection_a.join();
  connection_b.join();
  return succeeded.load(std::memory_order_relaxed);
}

void ExpectTaskListOrder(a2a::server::stores::PostgresTaskStore& store) {
  a2a::server::ListTasksRequest request;
  const auto all_tasks = store.List(request);
  ASSERT_TRUE(all_tasks.ok());
  ASSERT_EQ(all_tasks.value().tasks.size(), kConcurrentIds.size());
  for (std::size_t index = 0; index < kConcurrentIds.size(); ++index) {
    EXPECT_EQ(all_tasks.value().tasks[index].id(), kConcurrentIds[index]);
  }
}

void ExpectTaskPaginationOrder(a2a::server::stores::PostgresTaskStore& store) {
  a2a::server::ListTasksRequest request;
  request.page_size = kConcurrentPageSize;
  for (const std::string_view expected_id : kConcurrentIds) {
    const auto page = store.List(request);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().tasks.size(), kConcurrentPageSize);
    EXPECT_EQ(page.value().tasks.front().id(), expected_id);
    request.page_token = page.value().next_page_token;
  }
  EXPECT_TRUE(request.page_token.empty());
}

void ExpectPushConfigListOrder(a2a::server::stores::PostgresPushNotificationStore& store, std::string_view task_id) {
  const auto all_configs = store.List(task_id);
  ASSERT_TRUE(all_configs.ok());
  ASSERT_EQ(all_configs.value().configs_size(), static_cast<int>(kConcurrentIds.size()));
  for (std::size_t index = 0; index < kConcurrentIds.size(); ++index) {
    EXPECT_EQ(all_configs.value().configs(static_cast<int>(index)).id(), kConcurrentIds[index]);
  }
}

void ExpectPushConfigPaginationOrder(a2a::server::stores::PostgresPushNotificationStore& store,
                                     std::string_view task_id) {
  std::string page_token;
  for (const std::string_view expected_id : kConcurrentIds) {
    const auto page = store.List(task_id, static_cast<int>(kConcurrentPageSize), page_token);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().configs_size(), static_cast<int>(kConcurrentPageSize));
    EXPECT_EQ(page.value().configs(0).id(), expected_id);
    page_token = page.value().next_page_token();
  }
  EXPECT_TRUE(page_token.empty());
}

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

TEST(StoreConformanceTest, PostgresConnectionPoolRejectsZeroSizeBeforeConnecting) {
  constexpr std::string_view kInvalidConnectionString = "postgresql://invalid-host.invalid/a2a";

  EXPECT_THROW((void)a2a::server::stores::PostgresConnectionPool(std::string(kInvalidConnectionString), 0),
               std::invalid_argument);
}

TEST(StoreConformanceTest, PostgresOptionsDefaultToFourConnections) {
  const a2a::server::stores::PostgresStoreOptions options;

  EXPECT_EQ(options.connection_pool_size, a2a::server::stores::kDefaultPostgresConnectionPoolSize);
  EXPECT_EQ(options.connection_pool_size, 4U);
}

TEST(StoreConformanceTest, PostgresFactoryRejectsZeroPoolSizeBeforeConnecting) {
  constexpr std::string_view kInvalidConnectionString = "postgresql://invalid-host.invalid/a2a";
  const a2a::server::stores::PostgresStoreFactory factory(
      {.connection_string = std::string(kInvalidConnectionString), .connection_pool_size = 0U});

  const auto result = factory.CreateStoreBundle();

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_EQ(result.error().message(), a2a::server::stores::kPostgresConnectionPoolSizeValidationMessage);
}

TEST(StoreConformanceTest, PostgresBundleSharesConfiguredConnectionPool) {
  constexpr std::size_t kCustomPoolSize = 2U;
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreFactory factory({.connection_string = dsn_value,
                                                           .schema = MakePostgresTestSchema("shared_pool"),
                                                           .connection_pool_size = kCustomPoolSize});

  auto bundle = factory.CreateStoreBundle();

  ASSERT_TRUE(bundle.ok());
  const auto* task_store = dynamic_cast<a2a::server::stores::PostgresTaskStore*>(bundle.value().task_store.get());
  const auto* push_store =
      dynamic_cast<a2a::server::stores::PostgresPushNotificationStore*>(bundle.value().push_store.get());
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(push_store, nullptr);
  ASSERT_EQ(task_store->connection_pool_for_testing(), push_store->connection_pool_for_testing());
  EXPECT_EQ(task_store->connection_pool_for_testing()->capacity(), kCustomPoolSize);
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

TEST(StoreConformanceTest, PostgresTaskPaginationPreservesCreationOrderAcrossConnections) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("task_concurrent_order");
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn, .schema = schema};
  std::array<a2a::server::stores::PostgresTaskStore, 2> stores = {a2a::server::stores::PostgresTaskStore(options),
                                                                  a2a::server::stores::PostgresTaskStore(options)};
  ASSERT_TRUE(RunDeterministicallyInterleavedInserts([&](std::size_t connection, std::string_view id) {
    return stores[connection]
        .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
            std::string(id), "concurrent-context", lf::a2a::v1::TASK_STATE_WORKING, kOldTargetTaskTimestampSeconds))
        .ok();
  }));
  ExpectTaskListOrder(stores.front());
  ExpectTaskPaginationOrder(stores.front());
}

TEST(StoreConformanceTest, PostgresPushConfigPaginationPreservesCreationOrderAcrossConnections) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("push_concurrent_order");
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn, .schema = schema};
  std::array<a2a::server::stores::PostgresPushNotificationStore, 2> stores = {
      a2a::server::stores::PostgresPushNotificationStore(options),
      a2a::server::stores::PostgresPushNotificationStore(options)};
  constexpr std::string_view kTaskId = "concurrent-push-task";
  ASSERT_TRUE(RunDeterministicallyInterleavedInserts([&](std::size_t connection, std::string_view id) {
    return stores[connection]
        .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kTaskId), std::string(id)))
        .ok();
  }));
  ExpectPushConfigListOrder(stores.front(), kTaskId);
  ExpectPushConfigPaginationOrder(stores.front(), kTaskId);
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
