// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/push_notification_service.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/server/tasks/in_memory_task_store.h"

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
constexpr int kAcceptedHttpStatus = a2a::core::http::kStatusAccepted;
constexpr int kRejectedHttpStatus = a2a::core::http::kStatusInternalServerError;
constexpr int kCreatedConfigCount = 2;
constexpr int kRemainingConfigCount = 1;
constexpr int kEmptyConfigCount = 0;
constexpr int kSinglePageConfigCount = 1;
constexpr int kPushConfigPageSize = 1;
constexpr int kMissingHttpStatus = 0;
constexpr std::string_view kDeliveryFailureMessage = "delivery failed";
constexpr std::string_view kWebhookRejectedMessage = "webhook rejected task update";

class RecordingDeliveryClient final : public a2a::server::PushNotificationDeliveryClient {
 public:
  a2a::core::Result<a2a::server::PushDeliveryResult> Deliver(const a2a::server::PushDeliveryRequest& request) override {
    requests.push_back(request);
    if (fail_delivery) {
      return a2a::core::Error::Network(std::string(kDeliveryFailureMessage));
    }
    if (return_delivery_error_result) {
      return a2a::server::PushDeliveryResult{.http_status = kRejectedHttpStatus,
                                             .error_message = std::string(kWebhookRejectedMessage)};
    }
    return a2a::server::PushDeliveryResult{.http_status = delivery_http_status, .error_message = {}};
  }

  bool fail_delivery = false;
  bool return_delivery_error_result = false;
  int delivery_http_status = kAcceptedHttpStatus;
  std::vector<a2a::server::PushDeliveryRequest> requests;
};

class TaskAwareRecordingPushStore final : public a2a::server::PushNotificationStore {
 public:
  [[nodiscard]] a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdate(
      const lf::a2a::v1::TaskPushNotificationConfig& config) override {
    return store_.CreateOrUpdate(config);
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> Get(
      std::string_view task_id, std::string_view config_id) const override {
    return store_.Get(task_id, config_id);
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> List(
      std::string_view task_id, int page_size, std::string_view page_token) const override {
    ++list_calls;
    return store_.List(task_id, page_size, page_token);
  }

  [[nodiscard]] a2a::core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListForExistingTask(
      std::string_view task_id, int page_size, std::string_view page_token) const override {
    ++existing_task_list_calls;
    return store_.List(task_id, page_size, page_token);
  }

  [[nodiscard]] a2a::core::Result<void> Delete(std::string_view task_id, std::string_view config_id) override {
    return store_.Delete(task_id, config_id);
  }

  mutable int list_calls = 0;
  mutable int existing_task_list_calls = 0;

 private:
  a2a::server::InMemoryPushNotificationStore store_;
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

void ExpectPushConfigValidationError(const a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>& result) {
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_FALSE(result.error().protocol_code().has_value());
}

void ExpectPushConfigTaskNotFound(const a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>& result) {
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
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

TEST(PushNotificationServiceTest, CreateConfigPreservesTaskFirstErrorSemantics) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  auto config = BuildConfig(kConfigId);
  config.clear_id();

  const auto missing_task = service.CreateConfig(config);
  ExpectPushConfigTaskNotFound(missing_task);

  ASSERT_TRUE(task_store.CreateOrUpdate(BuildTask()).ok());
  const auto invalid_config = service.CreateConfig(config);
  ExpectPushConfigValidationError(invalid_config);
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

TEST(PushNotificationServiceTest, ListConfigRequiresExistingTask) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
  request.set_task_id(std::string(kTaskId));

  const auto listed = service.ListConfigs(request);

  ASSERT_FALSE(listed.ok());
  EXPECT_EQ(listed.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
  ASSERT_TRUE(listed.error().protocol_code().has_value());
  EXPECT_EQ(listed.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
}

TEST(PushNotificationServiceTest, ListConfigUsesConfiguredTaskStoreForTaskValidation) {
  a2a::server::InMemoryTaskStore task_store;
  TaskAwareRecordingPushStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  ASSERT_TRUE(push_store.CreateOrUpdate(BuildConfig(kConfigId)).ok());

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
  request.set_task_id(std::string(kTaskId));

  const auto missing = service.ListConfigs(request);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
  EXPECT_EQ(push_store.list_calls, 0);
  EXPECT_EQ(push_store.existing_task_list_calls, 0);

  ASSERT_TRUE(task_store.CreateOrUpdate(BuildTask()).ok());
  const auto listed = service.ListConfigs(request);
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value().configs_size(), kSinglePageConfigCount);
  EXPECT_EQ(push_store.list_calls, 0);
  EXPECT_EQ(push_store.existing_task_list_calls, 1);
}

TEST(PushNotificationServiceTest, ListConfigPassesPaginationToStore) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  ASSERT_TRUE(task_store.CreateOrUpdate(BuildTask()).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kConfigId)).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kOtherConfigId)).ok());

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest first_request;
  first_request.set_task_id(std::string(kTaskId));
  first_request.set_page_size(kPushConfigPageSize);
  const auto first_page = service.ListConfigs(first_request);
  ASSERT_TRUE(first_page.ok());
  ASSERT_EQ(first_page.value().configs_size(), kSinglePageConfigCount);
  ASSERT_FALSE(first_page.value().next_page_token().empty());

  lf::a2a::v1::ListTaskPushNotificationConfigsRequest second_request;
  second_request.set_task_id(std::string(kTaskId));
  second_request.set_page_size(kPushConfigPageSize);
  second_request.set_page_token(first_page.value().next_page_token());
  const auto second_page = service.ListConfigs(second_request);
  ASSERT_TRUE(second_page.ok());
  EXPECT_EQ(second_page.value().configs_size(), kSinglePageConfigCount);
  EXPECT_TRUE(second_page.value().next_page_token().empty());
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

TEST(PushNotificationServiceTest, NotifyTaskUpdatedPropagatesDeliveryFailures) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  const auto task = BuildTask();
  ASSERT_TRUE(task_store.CreateOrUpdate(task).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kConfigId)).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kOtherConfigId)).ok());
  delivery.fail_delivery = true;

  const auto notify = service.NotifyTaskUpdated(task);

  ASSERT_FALSE(notify.ok());
  EXPECT_EQ(notify.error().code(), a2a::core::ErrorCode::kNetwork);
  EXPECT_EQ(notify.error().message(), kDeliveryFailureMessage);
  ASSERT_EQ(delivery.requests.size(), static_cast<std::size_t>(kRemainingConfigCount));
  EXPECT_EQ(delivery.requests.front().config.id(), kConfigId);
}

TEST(PushNotificationServiceTest, NotifyTaskUpdatedPropagatesDeliveryResultErrors) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  const auto task = BuildTask();
  ASSERT_TRUE(task_store.CreateOrUpdate(task).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kConfigId)).ok());
  delivery.return_delivery_error_result = true;

  const auto notify = service.NotifyTaskUpdated(task);

  ASSERT_FALSE(notify.ok());
  EXPECT_EQ(notify.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
  EXPECT_EQ(notify.error().message(), kWebhookRejectedMessage);
  ASSERT_TRUE(notify.error().http_status().has_value());
  EXPECT_EQ(notify.error().http_status().value_or(kMissingHttpStatus), kRejectedHttpStatus);
  ASSERT_EQ(delivery.requests.size(), static_cast<std::size_t>(kRemainingConfigCount));
  EXPECT_EQ(delivery.requests.front().config.id(), kConfigId);
}

TEST(PushNotificationServiceTest, NotifyTaskUpdatedRejectsNonSuccessDeliveryStatus) {
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::InMemoryPushNotificationStore push_store;
  RecordingDeliveryClient delivery;
  a2a::server::PushNotificationService service(&task_store, &push_store, &delivery);
  const auto task = BuildTask();
  ASSERT_TRUE(task_store.CreateOrUpdate(task).ok());
  ASSERT_TRUE(service.CreateConfig(BuildConfig(kConfigId)).ok());
  delivery.delivery_http_status = kRejectedHttpStatus;

  const auto notify = service.NotifyTaskUpdated(task);

  ASSERT_FALSE(notify.ok());
  EXPECT_EQ(notify.error().code(), a2a::core::ErrorCode::kRemoteProtocol);
  ASSERT_TRUE(notify.error().http_status().has_value());
  EXPECT_EQ(notify.error().http_status().value_or(kMissingHttpStatus), kRejectedHttpStatus);
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
