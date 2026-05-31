// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_store.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kTaskId = "task-1";
constexpr std::string_view kOtherTaskId = "task-2";
constexpr std::string_view kConfigId = "push-1";
constexpr std::string_view kOtherConfigId = "push-2";
constexpr std::string_view kWebhookUrl = "http://127.0.0.1/webhook";
constexpr std::string_view kUpdatedWebhookUrl = "http://127.0.0.1/updated";
constexpr int kBulkConfigCount = 1000;

struct PushConfigIds final {
  std::string_view task_id;
  std::string_view config_id;
};

lf::a2a::v1::TaskPushNotificationConfig BuildConfig(PushConfigIds ids) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(ids.task_id));
  config.set_id(std::string(ids.config_id));
  config.set_url(std::string(kWebhookUrl));
  return config;
}

[[nodiscard]] std::vector<std::string> ConfigIds(const lf::a2a::v1::ListTaskPushNotificationConfigsResponse& response) {
  std::vector<std::string> ids;
  ids.reserve(static_cast<std::size_t>(response.configs_size()));
  for (const auto& config : response.configs()) {
    ids.push_back(config.id());
  }
  return ids;
}

[[nodiscard]] bool ContainsId(const std::vector<std::string>& ids, std::string_view expected_id) {
  return std::ranges::find(ids, expected_id) != ids.end();
}

void PopulateConfigs(a2a::server::InMemoryPushNotificationStore* store, int config_count) {
  for (int index = 0; index < config_count; ++index) {
    const std::string config_id = std::string(kConfigId) + std::to_string(index);
    ASSERT_TRUE(store->CreateOrUpdate(BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = config_id})).ok());
  }
}

}  // namespace

TEST(PushNotificationStoreTest, CreateGetListAndDeleteConfig) {
  a2a::server::InMemoryPushNotificationStore store;
  const auto created = store.CreateOrUpdate(BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = kConfigId}));
  ASSERT_TRUE(created.ok());

  const auto fetched = store.Get(kTaskId, kConfigId);
  ASSERT_TRUE(fetched.ok());
  EXPECT_EQ(fetched.value().id(), kConfigId);

  const auto listed = store.List(kTaskId);
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value().configs_size(), 1);
  EXPECT_EQ(listed.value().configs(0).task_id(), kTaskId);

  EXPECT_TRUE(store.Delete(kTaskId, kConfigId).ok());
  EXPECT_TRUE(store.Delete(kTaskId, kConfigId).ok());
  EXPECT_EQ(store.List(kTaskId).value().configs_size(), 0);
}

TEST(PushNotificationStoreTest, RejectsMissingRequiredFields) {
  a2a::server::InMemoryPushNotificationStore store;
  auto config = BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = kConfigId});

  config.clear_task_id();
  EXPECT_FALSE(store.CreateOrUpdate(config).ok());

  config = BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = kConfigId});
  config.clear_id();
  EXPECT_FALSE(store.CreateOrUpdate(config).ok());

  config = BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = kConfigId});
  config.clear_url();
  EXPECT_FALSE(store.CreateOrUpdate(config).ok());
}

TEST(PushNotificationStoreTest, PreservesMultipleConfigsAndScopesByTask) {
  a2a::server::InMemoryPushNotificationStore store;
  ASSERT_TRUE(store.CreateOrUpdate(BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = kConfigId})).ok());
  ASSERT_TRUE(store.CreateOrUpdate(BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = kOtherConfigId})).ok());
  ASSERT_TRUE(store.CreateOrUpdate(BuildConfig(PushConfigIds{.task_id = kOtherTaskId, .config_id = kConfigId})).ok());

  const auto first_task = store.List(kTaskId);
  ASSERT_TRUE(first_task.ok());
  EXPECT_EQ(first_task.value().configs_size(), 2);

  const auto other_task = store.List(kOtherTaskId);
  ASSERT_TRUE(other_task.ok());
  ASSERT_EQ(other_task.value().configs_size(), 1);
  EXPECT_EQ(other_task.value().configs(0).task_id(), kOtherTaskId);
}

TEST(PushNotificationStoreTest, UpdatingExistingConfigReplacesValueWithoutDuplicatingListEntry) {
  a2a::server::InMemoryPushNotificationStore store;
  auto config = BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = kConfigId});
  ASSERT_TRUE(store.CreateOrUpdate(config).ok());

  config.set_url(std::string(kUpdatedWebhookUrl));
  ASSERT_TRUE(store.CreateOrUpdate(config).ok());

  const auto fetched = store.Get(kTaskId, kConfigId);
  ASSERT_TRUE(fetched.ok());
  EXPECT_EQ(fetched.value().url(), kUpdatedWebhookUrl);
  const auto listed = store.List(kTaskId);
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value().configs_size(), 1);
}

TEST(PushNotificationStoreTest, LookupValidationAndMissingConfigBranchesAreCovered) {
  a2a::server::InMemoryPushNotificationStore store;
  ASSERT_TRUE(store.CreateOrUpdate(BuildConfig(PushConfigIds{.task_id = kTaskId, .config_id = kConfigId})).ok());

  EXPECT_FALSE(store.Get({}, kConfigId).ok());
  EXPECT_FALSE(store.Get(kTaskId, {}).ok());
  EXPECT_FALSE(store.Get(kOtherTaskId, kConfigId).ok());
  EXPECT_FALSE(store.Get(kTaskId, kOtherConfigId).ok());
  EXPECT_FALSE(store.List({}).ok());
  EXPECT_FALSE(store.Delete({}, kConfigId).ok());
  EXPECT_FALSE(store.Delete(kTaskId, {}).ok());
}

TEST(PushNotificationStoreTest, ListsLargeConfigSetAndDeleteOnlyRemovesTargetConfig) {
  a2a::server::InMemoryPushNotificationStore store;
  PopulateConfigs(&store, kBulkConfigCount);

  const std::string deleted_config_id = std::string(kConfigId) + std::to_string(kBulkConfigCount / 2);
  const std::string last_config_id = std::string(kConfigId) + std::to_string(kBulkConfigCount - 1);
  ASSERT_TRUE(store.Delete(kTaskId, deleted_config_id).ok());

  const auto moved_config = store.Get(kTaskId, last_config_id);
  ASSERT_TRUE(moved_config.ok());
  EXPECT_EQ(moved_config.value().id(), last_config_id);

  const auto listed = store.List(kTaskId);
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value().configs_size(), kBulkConfigCount - 1);
  const std::vector<std::string> ids = ConfigIds(listed.value());
  EXPECT_FALSE(ContainsId(ids, deleted_config_id));
}
