// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "a2a/server/push_notification_store.h"
#include "a2a/server/stores/postgres_common.h"
#include "a2a/server/stores/store_factory.h"

namespace a2a::server::stores {

constexpr std::string_view kPageTokenInvalidMessage =
    "ListTaskPushNotificationConfigsRequest.page_token must be a non-negative integer";
constexpr std::string_view kPageTokenOutOfRangeMessage =
    "ListTaskPushNotificationConfigsRequest.page_token exceeds available config count";
constexpr std::string_view kPushTaskIdRequiredMessage = "push notification task_id is required";
constexpr std::string_view kConfigIdRequiredMessage = "push notification id is required";
constexpr std::string_view kConfigUrlRequiredMessage = "push notification url is required";
constexpr std::string_view kTaskConfigNotFoundMessage = "push notification task config not found";
constexpr std::string_view kConfigNotFoundMessage = "push notification config not found";
constexpr std::string_view kPageSizeInvalidMessage =
    "ListTaskPushNotificationConfigsRequest.page_size must be non-negative";
constexpr std::size_t kPushListSqlReserveSlack = 512U;
constexpr std::size_t kPushUpsertSqlReserveSlack = 384U;

class PostgresConnectionPool;

class PostgresPushNotificationStore final : public a2a::server::PushNotificationStore {
 public:
  explicit PostgresPushNotificationStore(PostgresStoreOptions options);
  PostgresPushNotificationStore(std::shared_ptr<PostgresConnectionPool> pool, PostgresStoreOptions options);
  ~PostgresPushNotificationStore() override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdate(
      const lf::a2a::v1::TaskPushNotificationConfig& config) override;
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateOrUpdateForTask(
      const lf::a2a::v1::TaskPushNotificationConfig& config, const TaskStore& task_store) override;
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> Get(std::string_view task_id,
                                                                          std::string_view config_id) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetForTask(
      std::string_view task_id, std::string_view config_id, const TaskStore& task_store) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> List(
      std::string_view task_id, int page_size = 0, std::string_view page_token = {}) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListForExistingTask(
      std::string_view task_id, int page_size = 0, std::string_view page_token = {}) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListForTask(
      std::string_view task_id, int page_size, std::string_view page_token, const TaskStore& task_store) const override;
  [[nodiscard]] core::Result<void> Delete(std::string_view task_id, std::string_view config_id) override;
#ifdef A2A_POSTGRES_STORE_TESTING
  [[nodiscard]] const PostgresConnectionPool* connection_pool_for_testing() const noexcept;
  [[nodiscard]] core::Result<PostgresConnectionPool::Lease> AcquireConnectionForTesting();
  void SetSplitRoleAfterUpsertHookForTesting(std::function<void()> hook);
#endif

 private:
  enum class GetPath { kDirect, kTaskAware, kExistingTask };
  enum class UpsertPath { kLocalAtomic, kExternal, kSplitRoleLocal };
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> Upsert(
      const lf::a2a::v1::TaskPushNotificationConfig& config, UpsertPath path);
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> UpsertOnConnection(
      PGconn* connection, const lf::a2a::v1::TaskPushNotificationConfig& config, UpsertPath path);
  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetInternal(std::string_view task_id,
                                                                                  std::string_view config_id,
                                                                                  GetPath path) const;
  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListInternal(
      std::string_view task_id, int page_size, std::string_view page_token, bool validate_task_existence) const;

  std::shared_ptr<PostgresConnectionPool> pool_;
  PostgresStoreOptions options_;
  PostgresStorageIdentity storage_identity_;
  PostgresExecutionIdentity execution_identity_;
  std::string local_upsert_sql_;
  std::string external_upsert_sql_;
  std::string split_role_upsert_sql_;
  std::string task_aware_get_sql_;
  std::string existing_task_get_sql_;
  std::string task_config_exists_sql_;
  std::string task_aware_list_sql_;
  std::string existing_task_list_sql_;
  std::string delete_sql_;
#ifdef A2A_POSTGRES_STORE_TESTING
  std::function<void()> split_role_after_upsert_hook_for_testing_;
#endif
};

}  // namespace a2a::server::stores
