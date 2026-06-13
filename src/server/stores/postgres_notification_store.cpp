// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/postgres_notification_store.h"

#include <libpq-fe.h>

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"

namespace a2a::server::stores {
namespace {

[[nodiscard]] core::Result<std::size_t> ParsePushListPageToken(std::string_view page_token) {
  if (page_token.empty()) {
    return std::size_t{0};
  }
  std::size_t parsed = 0;
  const auto* begin = page_token.data();
  const auto* end = begin + page_token.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return core::Error::Validation(std::string(kPageTokenInvalidMessage));
  }
  return parsed;
}

[[nodiscard]] core::Result<void> ValidatePushConfig(const lf::a2a::v1::TaskPushNotificationConfig& config) {
  if (config.task_id().empty()) {
    return core::Error::Validation(std::string(kPushTaskIdRequiredMessage));
  }
  if (config.id().empty()) {
    return core::Error::Validation(std::string(kConfigIdRequiredMessage));
  }
  if (config.url().empty()) {
    return core::Error::Validation(std::string(kConfigUrlRequiredMessage));
  }
  return {};
}

[[nodiscard]] core::Result<void> ValidatePushLookup(std::string_view task_id, std::string_view config_id) {
  if (task_id.empty()) {
    return core::Error::Validation(std::string(kPushTaskIdRequiredMessage));
  }
  if (config_id.empty()) {
    return core::Error::Validation(std::string(kConfigIdRequiredMessage));
  }
  return {};
}

}  // namespace

PostgresPushNotificationStore::PostgresPushNotificationStore(PostgresStoreOptions options)
    : pool_(MakePool(options)), options_(std::move(options)) {
  auto lease = AcquireOrThrow(*pool_);
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
}

PostgresPushNotificationStore::PostgresPushNotificationStore(std::shared_ptr<PostgresConnectionPool> pool,
                                                             PostgresStoreOptions options)
    : pool_(std::move(pool)), options_(std::move(options)) {
  ValidatePostgresStoreOptionsOrThrow(options_);
  auto lease = AcquireOrThrow(*pool_);
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
}

PostgresPushNotificationStore::~PostgresPushNotificationStore() = default;

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::CreateOrUpdate(
    const lf::a2a::v1::TaskPushNotificationConfig& config) {
  const auto validation = ValidatePushConfig(config);
  if (!validation.ok()) {
    return validation.error();
  }
  const std::string payload = config.SerializeAsString();
  const std::string sql = "INSERT INTO " + PushTable(options_.schema) +
                          " (task_id, config_id, url, config_proto, updated_at) VALUES ($1, $2, $3, $4, now()) "
                          "ON CONFLICT (task_id, config_id) DO UPDATE SET url = EXCLUDED.url, "
                          "config_proto = EXCLUDED.config_proto, updated_at = now()";
  const char* values[] = {config.task_id().c_str(), config.id().c_str(), config.url().c_str(), payload.data()};
  const int lengths[] = {0, 0, 0, static_cast<int>(payload.size())};
  const int formats[] = {0, 0, 0, 1};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result(PQexecParams(lease.value().get(), sql.c_str(), 4, nullptr, values, lengths, formats, 0));
  const auto checked = CheckCommand(lease.value().get(), result.get(), "upsert postgres push notification config");
  if (!checked.ok()) {
    return checked.error();
  }
  return config;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::Get(
    std::string_view task_id, std::string_view config_id) const {
  const auto validation = ValidatePushLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }
  const std::string task_id_value(task_id);
  const std::string config_id_value(config_id);
  const std::string sql =
      "SELECT config_proto FROM " + PushTable(options_.schema) + " WHERE task_id = $1 AND config_id = $2";
  const char* values[] = {task_id_value.c_str(), config_id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result(PQexecParams(lease.value().get(), sql.c_str(), 2, nullptr, values, nullptr, nullptr, 1));
  const auto checked = CheckTuples(lease.value().get(), result.get(), "get postgres push notification config");
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) == 0) {
    const std::string exists_sql = "SELECT 1 FROM " + PushTable(options_.schema) + " WHERE task_id = $1 LIMIT 1";
    PgResult exists(PQexecParams(lease.value().get(), exists_sql.c_str(), 1, nullptr, &values[0], nullptr, nullptr, 0));
    const auto exists_checked =
        CheckTuples(lease.value().get(), exists.get(), "check postgres push notification task configs");
    if (!exists_checked.ok()) {
      return exists_checked.error();
    }
    if (PQntuples(exists.get()) == 0) {
      return core::protocol_errors::TaskNotFound(std::string(kTaskConfigNotFoundMessage));
    }
    return core::Error::Validation(std::string(kConfigNotFoundMessage));
  }
  lf::a2a::v1::TaskPushNotificationConfig config;
  if (!config.ParseFromArray(PQgetvalue(result.get(), 0, 0), PQgetlength(result.get(), 0, 0))) {
    return core::Error::Serialization("failed to parse stored TaskPushNotificationConfig protobuf");
  }
  return config;
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> PostgresPushNotificationStore::List(
    std::string_view task_id, int page_size, std::string_view page_token) const {
  if (task_id.empty()) {
    return core::Error::Validation(std::string(kPushTaskIdRequiredMessage));
  }
  if (page_size < 0) {
    return core::Error::Validation(std::string(kPageSizeInvalidMessage));
  }
  const auto offset = ParsePushListPageToken(page_token);
  if (!offset.ok()) {
    return offset.error();
  }
  const std::string task_id_value(task_id);
  const std::string sql =
      "SELECT config_proto FROM " + PushTable(options_.schema) + " WHERE task_id = $1 ORDER BY config_id ASC";
  const char* values[] = {task_id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result(PQexecParams(lease.value().get(), sql.c_str(), 1, nullptr, values, nullptr, nullptr, 1));
  const auto checked = CheckTuples(lease.value().get(), result.get(), "list postgres push notification configs");
  if (!checked.ok()) {
    return checked.error();
  }
  const auto count = static_cast<std::size_t>(PQntuples(result.get()));
  if (offset.value() > count) {
    return core::Error::Validation(std::string(kPageTokenOutOfRangeMessage));
  }
  const std::size_t remaining = count - offset.value();
  const std::size_t effective_page_size = page_size == 0 ? remaining : static_cast<std::size_t>(page_size);
  const std::size_t result_size = std::min(effective_page_size, remaining);
  lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
  response.mutable_configs()->Reserve(static_cast<int>(result_size));
  const std::size_t end = offset.value() + result_size;
  for (std::size_t index = offset.value(); index < end; ++index) {
    lf::a2a::v1::TaskPushNotificationConfig config;
    if (!config.ParseFromArray(PQgetvalue(result.get(), static_cast<int>(index), 0),
                               PQgetlength(result.get(), static_cast<int>(index), 0))) {
      return core::Error::Serialization("failed to parse stored TaskPushNotificationConfig protobuf");
    }
    *response.add_configs() = std::move(config);
  }
  if (result_size < remaining) {
    response.set_next_page_token(std::to_string(offset.value() + result_size));
  }
  return response;
}

core::Result<void> PostgresPushNotificationStore::Delete(std::string_view task_id, std::string_view config_id) {
  const auto validation = ValidatePushLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }
  const std::string task_id_value(task_id);
  const std::string config_id_value(config_id);
  const std::string sql = "DELETE FROM " + PushTable(options_.schema) + " WHERE task_id = $1 AND config_id = $2";
  const char* values[] = {task_id_value.c_str(), config_id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result(PQexecParams(lease.value().get(), sql.c_str(), 2, nullptr, values, nullptr, nullptr, 0));
  return CheckCommand(lease.value().get(), result.get(), "delete postgres push notification config");
}

}  // namespace a2a::server::stores
