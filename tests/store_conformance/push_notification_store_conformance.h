// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "a2a/server/push_notification_store.h"

namespace a2a::tests::store_conformance {

constexpr std::string_view kPushTask = "conformance-push-task";
constexpr std::string_view kPushConfig = "conformance-push-config";
constexpr std::string_view kOtherPushConfig = "conformance-push-config-2";
constexpr std::string_view kPushUrl = "https://example.test/webhook";
constexpr std::string_view kUpdatedPushUrl = "https://example.test/updated";

[[nodiscard]] inline lf::a2a::v1::TaskPushNotificationConfig MakeConfig(std::string task_id, std::string config_id,
                                                                        std::string url = std::string(kPushUrl)) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::move(task_id));
  config.set_id(std::move(config_id));
  config.set_url(std::move(url));
  return config;
}

template <typename Factory>
void RunPushNotificationStoreConformance(Factory&& factory) {
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

  EXPECT_TRUE(store->Delete(kPushTask, kPushConfig).ok());
  EXPECT_TRUE(store->Delete(kPushTask, kPushConfig).ok());
  EXPECT_FALSE(store->Get(kPushTask, kPushConfig).ok());

  EXPECT_FALSE(store->CreateOrUpdate(MakeConfig("", std::string(kPushConfig))).ok());
  EXPECT_FALSE(store->CreateOrUpdate(MakeConfig(std::string(kPushTask), "")).ok());
  EXPECT_FALSE(store->CreateOrUpdate(MakeConfig(std::string(kPushTask), std::string(kPushConfig), "")).ok());
  EXPECT_TRUE(store->List("missing-task").value().configs().empty());
}

}  // namespace a2a::tests::store_conformance
