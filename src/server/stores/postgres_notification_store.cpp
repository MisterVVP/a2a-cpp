// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/postgres_notification_store.h"

#include <libpq-fe.h>

#include <array>
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

[[nodiscard]] core::Result<std::size_t> ParsePushConfigCount(PGresult* result) {
  std::size_t parsed = 0;
  const std::string_view raw_count(PQgetvalue(result, 0, 1));
  const auto parsed_result = std::from_chars(raw_count.data(), raw_count.data() + raw_count.size(), parsed);
  if (parsed_result.ec != std::errc() || parsed_result.ptr != raw_count.data() + raw_count.size()) {
    return core::Error::Internal("count postgres push notification configs: failed to parse count");
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

#ifdef A2A_POSTGRES_STORE_TESTING
const PostgresConnectionPool* PostgresPushNotificationStore::connection_pool_for_testing() const noexcept {
  return pool_.get();
}
#endif

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
  constexpr int kPushUpsertParameterCount = 4;
  const std::array<const char*, kPushUpsertParameterCount> values = {config.task_id().c_str(), config.id().c_str(),
                                                                     config.url().c_str(), payload.data()};
  const std::array<int, kPushUpsertParameterCount> lengths = {0, 0, 0, static_cast<int>(payload.size())};
  const std::array<int, kPushUpsertParameterCount> formats = {0, 0, 0, 1};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kPushConfigUpsert);
#endif
    result.reset(
        PQexecParams(lease.value().get(), sql.c_str(), 4, nullptr, values.data(), lengths.data(), formats.data(), 0));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
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
  const std::array<const char*, 2> values = {task_id_value.c_str(), config_id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kPushConfigGet);
#endif
    result.reset(PQexecParams(lease.value().get(), sql.c_str(), 2, nullptr, values.data(), nullptr, nullptr, 1));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  const auto checked = CheckTuples(lease.value().get(), result.get(), "get postgres push notification config");
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) == 0) {
    const std::string exists_sql = "SELECT 1 FROM " + PushTable(options_.schema) + " WHERE task_id = $1 LIMIT 1";
    PgResult exists;
#ifdef A2A_POSTGRES_STORE_TESTING
    {
      const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kPushConfigGet);
#endif
      exists.reset(
          PQexecParams(lease.value().get(), exists_sql.c_str(), 1, nullptr, values.data(), nullptr, nullptr, 0));
#ifdef A2A_POSTGRES_STORE_TESTING
    }
#endif
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
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  const std::string limit_value = page_size == 0 ? std::string{} : std::to_string(page_size);
  const std::string offset_value = std::to_string(offset.value());
  const std::string sql = "WITH task AS MATERIALIZED (SELECT 1 FROM " + TaskTable(options_.schema) +
                          " WHERE task_id = $1), config_count AS MATERIALIZED (SELECT count(*) AS total FROM " +
                          PushTable(options_.schema) +
                          " WHERE task_id = $1) SELECT page.config_proto, config_count.total::text FROM task "
                          "CROSS JOIN config_count LEFT JOIN LATERAL (SELECT config_proto FROM " +
                          PushTable(options_.schema) +
                          " WHERE task_id = $1 ORDER BY created_sequence ASC LIMIT $2::bigint OFFSET $3::bigint) "
                          "AS page ON true";
  const std::array<const char*, 3> values = {task_id_value.c_str(), page_size == 0 ? nullptr : limit_value.c_str(),
                                             offset_value.c_str()};

  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kPushConfigListSelect);
#endif
    result.reset(PQexecParams(lease.value().get(), sql.c_str(), static_cast<int>(values.size()), nullptr, values.data(),
                              nullptr, nullptr, 1));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  const auto checked = CheckTuples(lease.value().get(), result.get(), "list postgres push notification configs");
  if (!checked.ok()) {
    return checked.error();
  }

  const auto result_rows = static_cast<std::size_t>(PQntuples(result.get()));
  if (result_rows == 0U) {
    return core::protocol_errors::TaskNotFound(std::string(kTaskConfigNotFoundMessage));
  }
  const auto count = ParsePushConfigCount(result.get());
  if (!count.ok()) {
    return count.error();
  }
  if (offset.value() > count.value()) {
    return core::Error::Validation(std::string(kPageTokenOutOfRangeMessage));
  }
  const std::size_t row_count = PQgetisnull(result.get(), 0, 0) != 0 ? 0U : result_rows;
  lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
  response.mutable_configs()->Reserve(static_cast<int>(row_count));
  for (std::size_t index = 0; index < row_count; ++index) {
    lf::a2a::v1::TaskPushNotificationConfig config;
    if (!config.ParseFromArray(PQgetvalue(result.get(), static_cast<int>(index), 0),
                               PQgetlength(result.get(), static_cast<int>(index), 0))) {
      return core::Error::Serialization("failed to parse stored TaskPushNotificationConfig protobuf");
    }
    *response.add_configs() = std::move(config);
  }
  if (offset.value() + row_count < count.value()) {
    response.set_next_page_token(std::to_string(offset.value() + row_count));
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
  const std::array<const char*, 2> values = {task_id_value.c_str(), config_id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kPushConfigDelete);
#endif
    result.reset(PQexecParams(lease.value().get(), sql.c_str(), 2, nullptr, values.data(), nullptr, nullptr, 0));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  return CheckCommand(lease.value().get(), result.get(), "delete postgres push notification config");
}

}  // namespace a2a::server::stores
