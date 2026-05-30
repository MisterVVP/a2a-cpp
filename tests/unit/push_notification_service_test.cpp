// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_service.h"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kTaskId = "task-1";
constexpr std::string_view kContextId = "ctx-1";
constexpr std::string_view kConfigId = "push-1";
constexpr std::string_view kOtherConfigId = "push-2";
constexpr std::string_view kWebhookUrl = "http://127.0.0.1/webhook";
constexpr std::string_view kToken = "token-1";
constexpr std::string_view kAuthScheme = "Bearer";
constexpr std::string_view kCredentials = "secret";
constexpr int kAcceptedHttpStatus = 202;

class RecordingDeliveryClient final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    requests.push_back(request);
    return a2a::server::PushDeliveryResult{.http_status = kAcceptedHttpStatus, .error_message = {}};
  }

  std::vector<a2a::server::PushDeliveryRequest> requests;
};

lf::a2a::v1::Task BuildTask() {
  lf::a2a::v1::Task task;
  task.set_id(std::string(kTaskId));
  task.set_context_id(std::string(kContextId));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);
  return task;
}

lf::a2a::v1::TaskPushNotificationConfig BuildConfig(std::string_view config_id) {
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(std::string(kTaskId));
  config.set_id(std::string(config_id));
  config.set_url(std::string(kWebhookUrl));
  config.set_token(std::string(kToken));
  config.mutable_authentication()->set_scheme(std::string(kAuthScheme));
  config.mutable_authentication()->set_credentials(std::string(kCredentials));
  return config;
}

lf::a2a::v1::SendMessageRequest BuildInlineRequest() {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id("message-1");
  *request.mutable_configuration()->mutable_task_push_notification_config() = BuildConfig(kConfigId);
  request.mutable_configuration()->mutable_task_push_notification_config()->clear_task_id();
  return request;
}

}  // namespace

TEST(PushNotificationServiceTest, CreateConfigRequiresExistingTask) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);

  EXPECT_FALSE(service.CreateConfig(BuildConfig(kConfigId)).ok());

  ASSERT_TRUE(task_store.CreateOrUpdate(BuildTask()).ok());
  EXPECT_TRUE(service.CreateConfig(BuildConfig(kConfigId)).ok());
}

TEST(PushNotificationServiceTest, InlineConfigResolvesTaskIdAndPreservesFields) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  ASSERT_TRUE(task_store.CreateOrUpdate(BuildTask()).ok());

  ASSERT_TRUE(service.RegisterInlineConfigIfPresent(BuildInlineRequest(), kTaskId).ok());
  const auto stored = push_store.Get(kTaskId, kConfigId);
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored.value().url(), kWebhookUrl);
  EXPECT_EQ(stored.value().token(), kToken);
  EXPECT_EQ(stored.value().authentication().scheme(), kAuthScheme);
  EXPECT_EQ(stored.value().authentication().credentials(), kCredentials);
}

TEST(PushNotificationServiceTest, NotifyTaskUpdatedDeliversStatusUpdateToEachConfig) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  const auto task = BuildTask();
  ASSERT_TRUE(task_store.CreateOrUpdate(task).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kConfigId)).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kOtherConfigId)).ok());

  ASSERT_TRUE(service.NotifyTaskUpdated(task).ok());
  ASSERT_EQ(delivery.requests.size(), 2);
  const auto& update = delivery.requests.front().payload.status_update();
  EXPECT_EQ(update.task_id(), kTaskId);
  EXPECT_EQ(update.context_id(), kContextId);
  EXPECT_EQ(update.status().state(), lf::a2a::v1::TASK_STATE_INPUT_REQUIRED);

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_task_id(std::string(kTaskId));
  delete_request.set_id(std::string(kConfigId));
  ASSERT_TRUE(service.DeleteConfig(delete_request).ok());
  delivery.requests.clear();
  ASSERT_TRUE(service.NotifyTaskUpdated(task).ok());
  ASSERT_EQ(delivery.requests.size(), 1);
  EXPECT_EQ(delivery.requests.front().config.id(), kOtherConfigId);
}
