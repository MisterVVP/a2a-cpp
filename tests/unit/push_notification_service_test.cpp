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
constexpr std::string_view kMessageId = "message-1";
constexpr int kAcceptedHttpStatus = 202;
constexpr int kCreatedConfigCount = 2;
constexpr int kRemainingConfigCount = 1;
constexpr int kEmptyConfigCount = 0;

class RecordingDeliveryClient final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    requests.push_back(request);
    if (fail_delivery) {
      return a2a::core::Error::Network("delivery failed");
    }
    return a2a::server::PushDeliveryResult{.http_status = kAcceptedHttpStatus, .error_message = {}};
  }

  bool fail_delivery = false;
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
  request.mutable_message()->set_message_id(std::string(kMessageId));
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

TEST(PushNotificationServiceTest, CreateConfigRequiresConfiguredStores) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;

  a2a::server::PushNotificationService missing_task_store(nullptr, &push_store, &delivery);
  EXPECT_FALSE(missing_task_store.CreateConfig(BuildConfig(kConfigId)).ok());

  a2a::server::PushNotificationService missing_push_store(&task_store, nullptr, &delivery);
  EXPECT_FALSE(missing_push_store.CreateConfig(BuildConfig(kConfigId)).ok());
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

TEST(PushNotificationServiceTest, InlineConfigFailsWhenResolvedTaskDoesNotExist) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);

  EXPECT_FALSE(service.RegisterInlineConfigIfPresent(BuildInlineRequest(), kTaskId).ok());
}

TEST(PushNotificationServiceTest, GetListAndDeleteConfigUseStore) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  ASSERT_TRUE(task_store.CreateOrUpdate(BuildTask()).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kConfigId)).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kOtherConfigId)).ok());

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_task_id(std::string(kTaskId));
  get_request.set_id(std::string(kConfigId));
  const auto fetched = service.GetConfig(get_request);
  ASSERT_TRUE(fetched.ok());
  EXPECT_EQ(fetched.value().id(), kConfigId);

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  list_request.set_task_id(std::string(kTaskId));
  const auto listed = service.ListConfigs(list_request);
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value().configs_size(), kCreatedConfigCount);

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_task_id(std::string(kTaskId));
  delete_request.set_id(std::string(kConfigId));
  ASSERT_TRUE(service.DeleteConfig(delete_request).ok());
  EXPECT_EQ(service.ListConfigs(list_request).value().configs_size(), kRemainingConfigCount);
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
  ASSERT_EQ(delivery.requests.size(), static_cast<std::size_t>(kCreatedConfigCount));
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
  ASSERT_EQ(delivery.requests.size(), static_cast<std::size_t>(kRemainingConfigCount));
  EXPECT_EQ(delivery.requests.front().config.id(), kOtherConfigId);
}

TEST(PushNotificationServiceTest, NotifyTaskUpdatedNoOpsWhenTaskHasNoConfigs) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);

  ASSERT_TRUE(service.NotifyTaskUpdated(BuildTask()).ok());
  EXPECT_TRUE(delivery.requests.empty());
}

TEST(PushNotificationServiceTest, NotifyTaskUpdatedIgnoresIndividualDeliveryFailures) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  const auto task = BuildTask();
  ASSERT_TRUE(task_store.CreateOrUpdate(task).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kConfigId)).ok());
  delivery.fail_delivery = true;

  EXPECT_TRUE(service.NotifyTaskUpdated(task).ok());
  ASSERT_EQ(delivery.requests.size(), static_cast<std::size_t>(kRemainingConfigCount));
  EXPECT_EQ(delivery.requests.front().config.id(), kConfigId);
}

TEST(PushNotificationServiceTest, StoreOperationsReturnConfigurationErrorsWhenStoreMissing) {
  a2a::server::InMemoryTaskStore task_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, nullptr, &delivery);

  lf::a2a::v1::GetTaskPushNotificationConfigRequest get_request;
  get_request.set_task_id(std::string(kTaskId));
  get_request.set_id(std::string(kConfigId));
  EXPECT_FALSE(service.GetConfig(get_request).ok());

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list_request;
  list_request.set_task_id(std::string(kTaskId));
  EXPECT_FALSE(service.ListConfigs(list_request).ok());

  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest delete_request;
  delete_request.set_task_id(std::string(kTaskId));
  delete_request.set_id(std::string(kConfigId));
  EXPECT_FALSE(service.DeleteConfig(delete_request).ok());
  EXPECT_FALSE(service.NotifyTaskUpdated(BuildTask()).ok());
}

TEST(PushNotificationServiceTest, NotifyTaskUpdatedRequiresDeliveryClient) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  a2a::server::PushNotificationService service(&task_store, &push_store, nullptr);

  EXPECT_FALSE(service.NotifyTaskUpdated(BuildTask()).ok());
}

TEST(PushNotificationServiceTest, RegisterInlineConfigNoOpsWhenRequestHasNoPushConfig) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id(std::string(kMessageId));
  ASSERT_TRUE(service.RegisterInlineConfigIfPresent(request, kTaskId).ok());

  const auto listed = push_store.List(kTaskId);
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value().configs_size(), kEmptyConfigCount);
}
