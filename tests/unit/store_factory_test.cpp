// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/store_factory.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "a2a/core/error.h"

namespace {

constexpr std::string_view kTaskId = "factory-task";
constexpr std::string_view kContextId = "factory-context";
constexpr std::string_view kPushConfigId = "factory-config";
constexpr std::string_view kPushUrl = "https://example.test/factory-webhook";
constexpr std::string_view kInvalidPostgresDsn = "postgresql://invalid-host.invalid/a2a";
constexpr std::string_view kInvalidSchema = "bad-schema";
constexpr std::string_view kPostgresNotBuiltMessage =
    "PostgreSQL store backend was not built; rebuild with A2A_ENABLE_POSTGRES_STORE=ON";

[[nodiscard]] lf::a2a::v1::Task MakeTask() {
  lf::a2a::v1::Task task;
  task.set_id(std::string(kTaskId));
  task.set_context_id(std::string(kContextId));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  return task;
}

[[nodiscard]] lf::a2a::v1::TaskPushNotificationConfig MakeConfig() {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(kTaskId));
  config.set_id(std::string(kPushConfigId));
  config.set_url(std::string(kPushUrl));
  return config;
}

TEST(StoreFactoryTest, InMemoryFactoryCreatesStoreFamilyThroughAbstractInterface) {
  const a2a::server::stores::InMemoryStoreFactory concrete_factory;
  const a2a::server::stores::StoreFactory& factory = concrete_factory;

  EXPECT_EQ(factory.backend_kind(), a2a::server::stores::StoreBackendKind::kInMemory);

  auto task_store = factory.CreateTaskStore();
  ASSERT_TRUE(task_store.ok());
  ASSERT_TRUE(task_store.value()->CreateOrUpdate(MakeTask()).ok());
  const auto fetched_task = task_store.value()->Get(kTaskId);
  ASSERT_TRUE(fetched_task.ok());
  EXPECT_EQ(fetched_task.value().context_id(), kContextId);

  auto push_store = factory.CreatePushNotificationStore();
  ASSERT_TRUE(push_store.ok());
  ASSERT_TRUE(push_store.value()->CreateOrUpdate(MakeConfig()).ok());
  const auto fetched_config = push_store.value()->Get(kTaskId, kPushConfigId);
  ASSERT_TRUE(fetched_config.ok());
  EXPECT_EQ(fetched_config.value().url(), kPushUrl);
}

TEST(StoreFactoryTest, InMemoryFactoryCreatesCompleteBundle) {
  const a2a::server::stores::InMemoryStoreFactory factory;

  auto bundle = factory.CreateStoreBundle();

  ASSERT_TRUE(bundle.ok());
  EXPECT_NE(bundle.value().task_store, nullptr);
  EXPECT_NE(bundle.value().push_store, nullptr);
}

TEST(StoreFactoryTest, InMemoryConvenienceFunctionDelegatesToFactory) {
  auto bundle = a2a::server::stores::CreateInMemoryStoreBundle();

  ASSERT_TRUE(bundle.ok());
  EXPECT_NE(bundle.value().task_store, nullptr);
  EXPECT_NE(bundle.value().push_store, nullptr);
}

#ifdef A2A_ENABLE_POSTGRES_STORE
TEST(StoreFactoryTest, PostgresFactoryRejectsInvalidSchemaBeforeConnecting) {
  const a2a::server::stores::PostgresStoreFactory factory(
      {.connection_string = std::string(kInvalidPostgresDsn), .schema = std::string(kInvalidSchema)});

  EXPECT_EQ(factory.backend_kind(), a2a::server::stores::StoreBackendKind::kPostgres);
  EXPECT_EQ(factory.options().schema, kInvalidSchema);

  const auto task_store = factory.CreateTaskStore();
  ASSERT_FALSE(task_store.ok());
  EXPECT_EQ(task_store.error().code(), a2a::core::ErrorCode::kValidation);

  const auto push_store = factory.CreatePushNotificationStore();
  ASSERT_FALSE(push_store.ok());
  EXPECT_EQ(push_store.error().code(), a2a::core::ErrorCode::kValidation);

  const auto bundle = factory.CreateStoreBundle();
  ASSERT_FALSE(bundle.ok());
  EXPECT_EQ(bundle.error().code(), a2a::core::ErrorCode::kValidation);
}
#else
TEST(StoreFactoryTest, PostgresFactoryReturnsClearErrorWhenBackendIsNotBuilt) {
  const a2a::server::stores::PostgresStoreFactory factory(
      {.connection_string = std::string(kInvalidPostgresDsn), .schema = "public"});

  EXPECT_EQ(factory.backend_kind(), a2a::server::stores::StoreBackendKind::kPostgres);

  const auto task_store = factory.CreateTaskStore();
  ASSERT_FALSE(task_store.ok());
  EXPECT_EQ(task_store.error().code(), a2a::core::ErrorCode::kInternal);
  EXPECT_EQ(task_store.error().message(), kPostgresNotBuiltMessage);

  const auto push_store = factory.CreatePushNotificationStore();
  ASSERT_FALSE(push_store.ok());
  EXPECT_EQ(push_store.error().code(), a2a::core::ErrorCode::kInternal);
  EXPECT_EQ(push_store.error().message(), kPostgresNotBuiltMessage);

  const auto bundle = factory.CreateStoreBundle();
  ASSERT_FALSE(bundle.ok());
  EXPECT_EQ(bundle.error().code(), a2a::core::ErrorCode::kInternal);
  EXPECT_EQ(bundle.error().message(), kPostgresNotBuiltMessage);
}
#endif

}  // namespace
