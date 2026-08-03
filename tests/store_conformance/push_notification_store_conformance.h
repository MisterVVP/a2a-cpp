// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "a2a/server/push_notification_store.h"

namespace a2a::tests::store_conformance {

constexpr std::string_view kPushTask = "conformance-push-task";
constexpr std::string_view kPushConfig = "conformance-push-config";
constexpr std::string_view kOtherPushConfig = "conformance-push-config-2";
constexpr std::string_view kOrderedPushTask = "conformance-push-order-task";
constexpr std::string_view kOrderedPushConfigFirst = "conformance-push-config-b";
constexpr std::string_view kOrderedPushConfigSecond = "conformance-push-config-a";
constexpr std::string_view kOrderedPushConfigThird = "conformance-push-config-c";
constexpr std::string_view kPushUrl = "https://example.test/webhook";
constexpr std::string_view kUpdatedPushUrl = "https://example.test/updated";

enum class MissingTaskListBehavior {
  kReturnsEmptyList,
  kReturnsTaskNotFound,
};

[[nodiscard]] inline lf::a2a::v1::TaskPushNotificationConfig MakeConfig(std::string task_id, std::string config_id,
                                                                        std::string url = std::string(kPushUrl)) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::move(task_id));
  config.set_id(std::move(config_id));
  config.set_url(std::move(url));
  return config;
}

template <typename Factory>
void RunPushNotificationStoreConformance(
    Factory&& factory, MissingTaskListBehavior missing_task_behavior = MissingTaskListBehavior::kReturnsEmptyList) {
  auto store = factory();
  const auto created = store->CreateOrUpdate(MakeConfig(std::string(kPushTask), std::string(kPushConfig)));
  ASSERT_TRUE(created.ok());
  auto updated = MakeConfig(std::string(kPushTask), std::string(kPushConfig), std::string(kUpdatedPushUrl));
  ASSERT_TRUE(store->CreateOrUpdate(updated).ok());

  const auto fetched = store->Get(kPushTask, kPushConfig);
  ASSERT_TRUE(fetched.ok());
  EXPECT_EQ(fetched.value().url(), kUpdatedPushUrl);

  ASSERT_TRUE(store->CreateOrUpdate(MakeConfig(std::string(kPushTask), std::string(kOtherPushConfig))).ok());
  const auto listed = store->List(kPushTask);
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value().configs_size(), 2);

  ASSERT_TRUE(
      store->CreateOrUpdate(MakeConfig(std::string(kOrderedPushTask), std::string(kOrderedPushConfigFirst))).ok());
  ASSERT_TRUE(
      store->CreateOrUpdate(MakeConfig(std::string(kOrderedPushTask), std::string(kOrderedPushConfigSecond))).ok());
  ASSERT_TRUE(
      store->CreateOrUpdate(MakeConfig(std::string(kOrderedPushTask), std::string(kOrderedPushConfigThird))).ok());

  const auto ordered = store->List(kOrderedPushTask);
  ASSERT_TRUE(ordered.ok());
  ASSERT_EQ(ordered.value().configs_size(), 3);
  EXPECT_EQ(ordered.value().configs(0).id(), kOrderedPushConfigFirst);
  EXPECT_EQ(ordered.value().configs(1).id(), kOrderedPushConfigSecond);
  EXPECT_EQ(ordered.value().configs(2).id(), kOrderedPushConfigThird);

  const auto first_page = store->List(kOrderedPushTask, 1);
  ASSERT_TRUE(first_page.ok());
  ASSERT_EQ(first_page.value().configs_size(), 1);
  EXPECT_EQ(first_page.value().configs(0).id(), kOrderedPushConfigFirst);
  ASSERT_FALSE(first_page.value().next_page_token().empty());

  const auto second_page = store->List(kOrderedPushTask, 1, first_page.value().next_page_token());
  ASSERT_TRUE(second_page.ok());
  ASSERT_EQ(second_page.value().configs_size(), 1);
  EXPECT_EQ(second_page.value().configs(0).id(), kOrderedPushConfigSecond);
  ASSERT_FALSE(second_page.value().next_page_token().empty());

  const auto third_page = store->List(kOrderedPushTask, 1, second_page.value().next_page_token());
  ASSERT_TRUE(third_page.ok());
  ASSERT_EQ(third_page.value().configs_size(), 1);
  EXPECT_EQ(third_page.value().configs(0).id(), kOrderedPushConfigThird);
  EXPECT_TRUE(third_page.value().next_page_token().empty());

  EXPECT_TRUE(store->Delete(kOrderedPushTask, kOrderedPushConfigFirst).ok());
  const auto ordered_after_delete = store->List(kOrderedPushTask);
  ASSERT_TRUE(ordered_after_delete.ok());
  ASSERT_EQ(ordered_after_delete.value().configs_size(), 2);
  EXPECT_EQ(ordered_after_delete.value().configs(0).id(), kOrderedPushConfigSecond);
  EXPECT_EQ(ordered_after_delete.value().configs(1).id(), kOrderedPushConfigThird);

  const auto page_after_delete = store->List(kOrderedPushTask, 1);
  ASSERT_TRUE(page_after_delete.ok());
  ASSERT_EQ(page_after_delete.value().configs_size(), 1);
  EXPECT_EQ(page_after_delete.value().configs(0).id(), kOrderedPushConfigSecond);
  ASSERT_FALSE(page_after_delete.value().next_page_token().empty());

  const auto second_page_after_delete = store->List(kOrderedPushTask, 1, page_after_delete.value().next_page_token());
  ASSERT_TRUE(second_page_after_delete.ok());
  ASSERT_EQ(second_page_after_delete.value().configs_size(), 1);
  EXPECT_EQ(second_page_after_delete.value().configs(0).id(), kOrderedPushConfigThird);
  EXPECT_TRUE(second_page_after_delete.value().next_page_token().empty());

  EXPECT_TRUE(store->Delete(kPushTask, kPushConfig).ok());
  EXPECT_TRUE(store->Delete(kPushTask, kPushConfig).ok());
  EXPECT_FALSE(store->Get(kPushTask, kPushConfig).ok());

  EXPECT_FALSE(store->CreateOrUpdate(MakeConfig("", std::string(kPushConfig))).ok());
  EXPECT_FALSE(store->CreateOrUpdate(MakeConfig(std::string(kPushTask), "")).ok());
  EXPECT_FALSE(store->CreateOrUpdate(MakeConfig(std::string(kPushTask), std::string(kPushConfig), "")).ok());
  const auto missing_task = store->List("missing-task");
  if (missing_task_behavior == MissingTaskListBehavior::kReturnsTaskNotFound) {
    EXPECT_FALSE(missing_task.ok());
  } else {
    ASSERT_TRUE(missing_task.ok());
    EXPECT_TRUE(missing_task.value().configs().empty());
  }
}

}  // namespace a2a::tests::store_conformance
