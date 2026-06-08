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
#include "a2a/server/stores/postgres_store.h"
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

[[nodiscard]] std::string MakePostgresTestSchema(std::string_view suffix) {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return "a2a_test_" + std::to_string(ticks) + "_" + std::string(suffix);
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
