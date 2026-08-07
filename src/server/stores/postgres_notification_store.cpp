// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/postgres_notification_store.h"

#include <libpq-fe.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/server/stores/postgres_task_store.h"

namespace a2a::server::stores {
namespace {

constexpr std::uint64_t kPostgresBigintMaximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
constexpr std::size_t kPushTaskAwareGetSqlReserveSlack = 192U;
constexpr std::size_t kPushExistingTaskGetSqlReserveSlack = 96U;
constexpr std::size_t kPushTaskConfigExistsSqlReserveSlack = 64U;
constexpr std::size_t kPushDeleteSqlReserveSlack = 64U;
constexpr std::string_view kPushListMissingCountMessage =
    "list postgres push notification configs: query returned no count row";
constexpr std::string_view kCheckPushTaskConfigsOperation = "check postgres push notification task configs";
constexpr std::string_view kValidatePostgresPushSchemaOperation = "validate postgres push notification schema";
constexpr std::string_view kPostgresPushSchemaMigrationRequiredMessage =
    "PostgreSQL push-notification schema is missing task-aware-push-config-v2; apply the migration in "
    "docs/storage.md before using auto_create_schema=false";
constexpr std::string_view kPushConfigProvenanceColumnName = "local_postgres_task";
constexpr std::string_view kPostgresAfterDeleteRowTriggerType = "9";
constexpr std::string_view kPostgresTriggerEnabledForOrigin = "O";
constexpr std::string_view kPostgresTriggerEnabledAlways = "A";
constexpr std::string_view kPostgresTrueValue = "t";
constexpr int kPostgresPushSchemaParameterCount = 11;
constexpr int kPostgresPushSchemaCheckCount = 9;
constexpr char kValidatePostgresPushSchemaSql[] =
    "WITH relations AS MATERIALIZED ("
    "SELECT schema_namespace.oid AS schema_oid, "
    "pg_catalog.to_regclass(pg_catalog.format('%I.%I', $1::text, $2::text)) AS push_oid, "
    "pg_catalog.to_regclass(pg_catalog.format('%I.%I', $1::text, $8::text)) AS task_oid "
    "FROM pg_catalog.pg_namespace AS schema_namespace WHERE schema_namespace.nspname = $1), "
    "lock_function AS MATERIALIZED ("
    "SELECT routine.oid, routine.proacl, routine.proowner, routine.prosecdef, routine.provolatile, "
    "routine.proconfig, routine.prorettype, "
    "pg_catalog.obj_description(routine.oid, 'pg_proc') AS migration_id FROM "
    "(SELECT pg_catalog.to_regprocedure(pg_catalog.format('%I.%I(text)', $1::text, $4::text)) AS oid) AS resolved "
    "LEFT JOIN pg_catalog.pg_proc AS routine ON routine.oid = resolved.oid), "
    "delete_function AS MATERIALIZED ("
    "SELECT routine.oid, routine.proacl, routine.proowner, routine.prosecdef, routine.provolatile, routine.proconfig, "
    "routine.prorettype, routine.prosrc, pg_catalog.obj_description(routine.oid, 'pg_proc') AS migration_id FROM "
    "(SELECT pg_catalog.to_regprocedure(pg_catalog.format('%I.%I()', $1::text, $6::text)) AS oid) AS resolved "
    "LEFT JOIN pg_catalog.pg_proc AS routine ON routine.oid = resolved.oid), "
    "cleanup_trigger AS MATERIALIZED ("
    "SELECT trigger.tgfoid, trigger.tgtype, trigger.tgenabled, trigger.tgnargs, trigger.tgqual "
    "FROM pg_catalog.pg_trigger AS trigger "
    "JOIN pg_catalog.pg_class AS relation ON relation.oid = trigger.tgrelid "
    "JOIN pg_catalog.pg_namespace AS relation_namespace ON relation_namespace.oid = relation.relnamespace "
    "WHERE relation_namespace.nspname = $1 AND relation.relname = $8 AND trigger.tgname = $7 "
    "AND NOT trigger.tgisinternal) "
    "SELECT "
    "EXISTS (SELECT 1 FROM pg_catalog.pg_attribute AS attribute "
    "JOIN pg_catalog.pg_attrdef AS column_default ON column_default.adrelid = attribute.attrelid "
    "AND column_default.adnum = attribute.attnum "
    "WHERE attribute.attrelid = relations.push_oid AND attribute.attname = $3 "
    "AND attribute.attnum > 0 AND NOT attribute.attisdropped "
    "AND attribute.atttypid = 'pg_catalog.bool'::pg_catalog.regtype AND attribute.attnotnull "
    "AND attribute.atthasdef "
    "AND pg_catalog.pg_get_expr(column_default.adbin, column_default.adrelid) = 'false'), "
    "relations.push_oid IS NOT NULL AND relations.task_oid IS NOT NULL AND NOT EXISTS ("
    "SELECT 1 FROM pg_catalog.pg_constraint AS foreign_key WHERE foreign_key.contype = 'f' "
    "AND foreign_key.conrelid = relations.push_oid AND foreign_key.confrelid = relations.task_oid), "
    "lock_function.oid IS NOT NULL AND lock_function.migration_id = $5 AND lock_function.prosecdef "
    "AND lock_function.provolatile = 'v' AND lock_function.prorettype = 'pg_catalog.bool'::pg_catalog.regtype "
    "AND 'search_path=pg_catalog' = ANY(lock_function.proconfig), "
    "CASE WHEN lock_function.oid IS NULL THEN FALSE ELSE NOT EXISTS ("
    "SELECT 1 FROM pg_catalog.aclexplode(COALESCE(lock_function.proacl, "
    "pg_catalog.acldefault('f', lock_function.proowner))) AS acl_entry "
    "WHERE acl_entry.grantee = 0 AND acl_entry.privilege_type = 'EXECUTE') END, "
    "lock_function.oid IS NOT NULL "
    "AND pg_catalog.has_schema_privilege(lock_function.proowner, relations.schema_oid, 'USAGE') "
    "AND pg_catalog.has_table_privilege(lock_function.proowner, relations.task_oid, 'SELECT') "
    "AND pg_catalog.has_table_privilege(lock_function.proowner, relations.task_oid, 'UPDATE'), "
    "delete_function.oid IS NOT NULL AND delete_function.migration_id = $5 AND delete_function.prosecdef "
    "AND delete_function.provolatile = 'v' "
    "AND delete_function.prorettype = 'pg_catalog.trigger'::pg_catalog.regtype "
    "AND 'search_path=pg_catalog' = ANY(delete_function.proconfig) "
    "AND pg_catalog.regexp_replace(pg_catalog.lower(delete_function.prosrc), '[[:space:]\"]+', '', 'g') = "
    "'begindeletefrom' || pg_catalog.lower($1::text) || '.' || pg_catalog.lower($2::text) || "
    "'wheretask_id=old.idandlocal_postgres_task;returnold;end', "
    "CASE WHEN delete_function.oid IS NULL THEN FALSE ELSE NOT EXISTS ("
    "SELECT 1 FROM pg_catalog.aclexplode(COALESCE(delete_function.proacl, "
    "pg_catalog.acldefault('f', delete_function.proowner))) AS acl_entry "
    "WHERE acl_entry.grantee = 0 AND acl_entry.privilege_type = 'EXECUTE') END, "
    "delete_function.oid IS NOT NULL "
    "AND pg_catalog.has_schema_privilege(delete_function.proowner, relations.schema_oid, 'USAGE') "
    "AND pg_catalog.has_table_privilege(delete_function.proowner, relations.push_oid, 'SELECT') "
    "AND pg_catalog.has_table_privilege(delete_function.proowner, relations.push_oid, 'DELETE'), "
    "EXISTS (SELECT 1 FROM cleanup_trigger WHERE cleanup_trigger.tgfoid = delete_function.oid "
    "AND cleanup_trigger.tgtype = $9::smallint "
    "AND (cleanup_trigger.tgenabled = $10::pg_catalog.\"char\" "
    "OR cleanup_trigger.tgenabled = $11::pg_catalog.\"char\") "
    "AND cleanup_trigger.tgnargs = 0 AND cleanup_trigger.tgqual IS NULL) "
    "FROM relations CROSS JOIN lock_function CROSS JOIN delete_function";

[[nodiscard]] core::Result<std::size_t> ParsePushListPageToken(std::string_view page_token) {
  if (page_token.empty()) {
    return std::size_t{0};
  }
  std::uint64_t parsed = 0;
  const auto* begin = page_token.data();
  const auto* end = begin + page_token.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec == std::errc::result_out_of_range) {
    return core::Error::Validation(std::string(kPageTokenOutOfRangeMessage));
  }
  if (result.ec != std::errc() || result.ptr != end) {
    return core::Error::Validation(std::string(kPageTokenInvalidMessage));
  }
  if (parsed > kPostgresBigintMaximum || parsed > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return core::Error::Validation(std::string(kPageTokenOutOfRangeMessage));
  }
  return static_cast<std::size_t>(parsed);
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

[[nodiscard]] std::string BuildPushListSql(std::string_view schema, bool validate_task_existence) {
  const std::string task_table = TaskTable(schema);
  const std::string push_table = PushTable(schema);
  std::string sql;
  sql.reserve(task_table.size() + push_table.size() + push_table.size() + kPushListSqlReserveSlack);
  sql.append("WITH ");
  if (validate_task_existence) {
    sql.append("task AS MATERIALIZED (SELECT 1 FROM ");
    sql.append(task_table);
    sql.append(" WHERE id = $1), ");
  }
  sql.append("config_count AS MATERIALIZED (SELECT count(*) AS total FROM ");
  sql.append(push_table);
  sql.append(" WHERE task_id = $1) SELECT page.config_proto, config_count.total::text FROM ");
  if (validate_task_existence) {
    sql.append("task CROSS JOIN ");
  }
  sql.append("config_count LEFT JOIN LATERAL (SELECT config_proto FROM ");
  sql.append(push_table);
  sql.append(
      " WHERE task_id = $1 AND $3::bigint <= config_count.total "
      "ORDER BY created_sequence ASC LIMIT $2::bigint OFFSET $3::bigint) AS page ON true");
  return sql;
}

[[nodiscard]] std::string BuildPushUpsertSql(std::string_view schema, bool validate_postgres_task) {
  const std::string push_table = PushTable(schema);
  const std::string task_lock_function = validate_postgres_task ? TaskPushConfigLockFunction(schema) : std::string{};
  std::string sql;
  sql.reserve(push_table.size() + task_lock_function.size() + kPushUpsertSqlReserveSlack);
  if (validate_postgres_task) {
    sql.append("WITH task AS MATERIALIZED (SELECT ");
    sql.append(task_lock_function);
    sql.append("($1) AS task_exists) ");
  }
  sql.append("INSERT INTO ");
  sql.append(push_table);
  sql.append(" (task_id, config_id, url, config_proto, local_postgres_task, updated_at) ");
  if (validate_postgres_task) {
    sql.append("SELECT $1, $2, $3, $4, TRUE, now() FROM task WHERE task.task_exists ");
  } else {
    sql.append("VALUES ($1, $2, $3, $4, FALSE, now()) ");
  }
  sql.append(
      "ON CONFLICT (task_id, config_id) DO UPDATE SET url = EXCLUDED.url, "
      "config_proto = EXCLUDED.config_proto, local_postgres_task = EXCLUDED.local_postgres_task, "
      "updated_at = now() ");
  sql.append("RETURNING 1");
  return sql;
}

[[nodiscard]] std::string BuildPushGetSql(std::string_view schema, bool validate_task_existence) {
  const std::string push_table = PushTable(schema);
  std::string sql;
  if (validate_task_existence) {
    const std::string task_table = TaskTable(schema);
    sql.reserve(task_table.size() + push_table.size() + kPushTaskAwareGetSqlReserveSlack);
    sql.append("SELECT config.config_proto FROM ");
    sql.append(task_table);
    sql.append(" AS task LEFT JOIN ");
    sql.append(push_table);
    sql.append(" AS config ON config.task_id = task.id AND config.config_id = $2 WHERE task.id = $1");
  } else {
    sql.reserve(push_table.size() + kPushExistingTaskGetSqlReserveSlack);
    sql.append("SELECT config_proto FROM ");
    sql.append(push_table);
    sql.append(" WHERE task_id = $1 AND config_id = $2");
  }
  return sql;
}

[[nodiscard]] std::string BuildPushTaskConfigExistsSql(std::string_view schema) {
  const std::string push_table = PushTable(schema);
  std::string sql;
  sql.reserve(push_table.size() + kPushTaskConfigExistsSqlReserveSlack);
  sql.append("SELECT 1 FROM ");
  sql.append(push_table);
  sql.append(" WHERE task_id = $1 LIMIT 1");
  return sql;
}

[[nodiscard]] core::Result<bool> PushTaskConfigCollectionExists(PGconn* connection, const std::string& sql,
                                                                const char* task_id) {
  const std::array<const char*, 1> values = {task_id};
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kPushConfigGet);
#endif
    result.reset(PQexecParams(connection, sql.c_str(), static_cast<int>(values.size()), nullptr, values.data(), nullptr,
                              nullptr, 0));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  const auto checked = CheckTuples(connection, result.get(), kCheckPushTaskConfigsOperation);
  if (!checked.ok()) {
    return checked.error();
  }
  return PQntuples(result.get()) != 0;
}

[[nodiscard]] std::string BuildPushDeleteSql(std::string_view schema) {
  const std::string push_table = PushTable(schema);
  std::string sql;
  sql.reserve(push_table.size() + kPushDeleteSqlReserveSlack);
  sql.append("DELETE FROM ");
  sql.append(push_table);
  sql.append(" WHERE task_id = $1 AND config_id = $2");
  return sql;
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

[[nodiscard]] core::Result<std::size_t> ValidatePushListRequest(std::string_view task_id, int page_size,
                                                                std::string_view page_token) {
  if (task_id.empty()) {
    return core::Error::Validation(std::string(kPushTaskIdRequiredMessage));
  }
  if (page_size < 0) {
    return core::Error::Validation(std::string(kPageSizeInvalidMessage));
  }
  return ParsePushListPageToken(page_token);
}

[[nodiscard]] bool IsPostgresTrue(PGresult* result, int column) {
  return PQgetisnull(result, 0, column) == 0 && std::string_view(PQgetvalue(result, 0, column)) == kPostgresTrueValue;
}

[[nodiscard]] core::Result<void> ValidateManagedPushSchema(PGconn* connection, const PostgresStoreOptions& options) {
  const std::array<const char*, kPostgresPushSchemaParameterCount> values = {options.schema.c_str(),
                                                                             kPushTableName.data(),
                                                                             kPushConfigProvenanceColumnName.data(),
                                                                             kTaskPushConfigLockFunction.data(),
                                                                             kTaskPushConfigMigrationId.data(),
                                                                             kDeleteTaskPushConfigsFunction.data(),
                                                                             kDeleteTaskPushConfigsTrigger.data(),
                                                                             kTaskTableName.data(),
                                                                             kPostgresAfterDeleteRowTriggerType.data(),
                                                                             kPostgresTriggerEnabledForOrigin.data(),
                                                                             kPostgresTriggerEnabledAlways.data()};
  PgResult result(PQexecParams(connection, kValidatePostgresPushSchemaSql, static_cast<int>(values.size()), nullptr,
                               values.data(), nullptr, nullptr, 0));
  const auto checked = CheckTuples(connection, result.get(), kValidatePostgresPushSchemaOperation);
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != kPostgresPushSchemaCheckCount) {
    return core::Error::Internal(std::string(kPostgresPushSchemaMigrationRequiredMessage));
  }
  for (int column = 0; column < kPostgresPushSchemaCheckCount; ++column) {
    if (!IsPostgresTrue(result.get(), column)) {
      return core::Error::Internal(std::string(kPostgresPushSchemaMigrationRequiredMessage));
    }
  }
  return {};
}

[[nodiscard]] core::Result<void> PreparePushSchema(PGconn* connection, const PostgresStoreOptions& options) {
  auto initialized = InitializeSchema(connection, options);
  if (!initialized.ok()) {
    return initialized;
  }
  return ValidateManagedPushSchema(connection, options);
}

}  // namespace

PostgresPushNotificationStore::PostgresPushNotificationStore(PostgresStoreOptions options)
    : pool_(MakePool(options)),
      options_(std::move(options)),
      storage_identity_(pool_->StorageCoordinates(options_.schema)),
      execution_identity_(pool_->ExecutionIdentity(options_.schema)),
      local_upsert_sql_(BuildPushUpsertSql(options_.schema, true)),
      external_upsert_sql_(BuildPushUpsertSql(options_.schema, false)),
      task_aware_get_sql_(BuildPushGetSql(options_.schema, true)),
      existing_task_get_sql_(BuildPushGetSql(options_.schema, false)),
      task_config_exists_sql_(BuildPushTaskConfigExistsSql(options_.schema)),
      task_aware_list_sql_(BuildPushListSql(options_.schema, true)),
      existing_task_list_sql_(BuildPushListSql(options_.schema, false)),
      delete_sql_(BuildPushDeleteSql(options_.schema)) {
  auto lease = AcquireOrThrow(*pool_);
  const auto prepared = PreparePushSchema(lease.get(), options_);
  if (!prepared.ok()) {
    throw std::runtime_error(std::string(prepared.error().message()));
  }
}

PostgresPushNotificationStore::PostgresPushNotificationStore(std::shared_ptr<PostgresConnectionPool> pool,
                                                             PostgresStoreOptions options)
    : pool_(std::move(pool)),
      options_(std::move(options)),
      storage_identity_(pool_->StorageCoordinates(options_.schema)),
      execution_identity_(pool_->ExecutionIdentity(options_.schema)),
      local_upsert_sql_(BuildPushUpsertSql(options_.schema, true)),
      external_upsert_sql_(BuildPushUpsertSql(options_.schema, false)),
      task_aware_get_sql_(BuildPushGetSql(options_.schema, true)),
      existing_task_get_sql_(BuildPushGetSql(options_.schema, false)),
      task_config_exists_sql_(BuildPushTaskConfigExistsSql(options_.schema)),
      task_aware_list_sql_(BuildPushListSql(options_.schema, true)),
      existing_task_list_sql_(BuildPushListSql(options_.schema, false)),
      delete_sql_(BuildPushDeleteSql(options_.schema)) {
  ValidatePostgresStoreOptionsOrThrow(options_);
  auto lease = AcquireOrThrow(*pool_);
  const auto prepared = PreparePushSchema(lease.get(), options_);
  if (!prepared.ok()) {
    throw std::runtime_error(std::string(prepared.error().message()));
  }
}

PostgresPushNotificationStore::~PostgresPushNotificationStore() = default;

#ifdef A2A_POSTGRES_STORE_TESTING
const PostgresConnectionPool* PostgresPushNotificationStore::connection_pool_for_testing() const noexcept {
  return pool_.get();
}

core::Result<PostgresConnectionPool::Lease> PostgresPushNotificationStore::AcquireConnectionForTesting() {
  return pool_->Acquire();
}
#endif

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::CreateOrUpdate(
    const lf::a2a::v1::TaskPushNotificationConfig& config) {
  const auto validation = ValidatePushConfig(config);
  if (!validation.ok()) {
    return validation.error();
  }
  return Upsert(config, UpsertPath::kExternal);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::Upsert(
    const lf::a2a::v1::TaskPushNotificationConfig& config, UpsertPath path) {
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  return UpsertOnConnection(lease.value().get(), config, path);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::UpsertOnConnection(
    PGconn* connection, const lf::a2a::v1::TaskPushNotificationConfig& config, UpsertPath path) {
  const std::string payload = config.SerializeAsString();
  const std::string& sql = path == UpsertPath::kLocalAtomic ? local_upsert_sql_ : external_upsert_sql_;
  constexpr int kPushUpsertParameterCount = 4;
  const std::array<const char*, kPushUpsertParameterCount> values = {config.task_id().c_str(), config.id().c_str(),
                                                                     config.url().c_str(), payload.data()};
  const std::array<int, kPushUpsertParameterCount> lengths = {0, 0, 0, static_cast<int>(payload.size())};
  const std::array<int, kPushUpsertParameterCount> formats = {0, 0, 0, 1};
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kPushConfigUpsert);
#endif
    result.reset(PQexecParams(connection, sql.c_str(), 4, nullptr, values.data(), lengths.data(), formats.data(), 0));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  const auto checked = CheckTuples(connection, result.get(), "upsert postgres push notification config");
  if (!checked.ok()) {
    return checked.error();
  }
  if (path == UpsertPath::kLocalAtomic && PQntuples(result.get()) == 0) {
    return core::protocol_errors::TaskNotFound(std::string(kTaskConfigNotFoundMessage));
  }
  return config;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::CreateOrUpdateForTask(
    const lf::a2a::v1::TaskPushNotificationConfig& config, const TaskStore& task_store) {
  const auto validation = ValidatePushConfig(config);
  if (!validation.ok()) {
    const auto task = task_store.Get(config.task_id());
    if (!task.ok()) {
      return task.error();
    }
    return validation.error();
  }
  const auto* postgres_task_store = dynamic_cast<const PostgresTaskStore*>(&task_store);
  if (postgres_task_store != nullptr && postgres_task_store->UsesStorage(storage_identity_)) {
    return Upsert(config, UpsertPath::kLocalAtomic);
  }
  const auto task = task_store.Get(config.task_id());
  if (!task.ok()) {
    return task.error();
  }
  return Upsert(config, UpsertPath::kExternal);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::Get(
    std::string_view task_id, std::string_view config_id) const {
  const auto validation = ValidatePushLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }
  return GetInternal(task_id, config_id, GetPath::kDirect);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::GetForTask(
    std::string_view task_id, std::string_view config_id, const TaskStore& task_store) const {
  const auto validation = ValidatePushLookup(task_id, config_id);
  if (!validation.ok()) {
    return validation.error();
  }
  const auto* postgres_task_store = dynamic_cast<const PostgresTaskStore*>(&task_store);
  if (postgres_task_store != nullptr && postgres_task_store->UsesExecutionIdentity(execution_identity_)) {
    return GetInternal(task_id, config_id, GetPath::kTaskAware);
  }
  const auto task = task_store.Get(task_id);
  if (!task.ok()) {
    return task.error();
  }
  return GetInternal(task_id, config_id, GetPath::kExistingTask);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> PostgresPushNotificationStore::GetInternal(
    std::string_view task_id, std::string_view config_id, GetPath path) const {
  const std::string task_id_value(task_id);
  const std::string config_id_value(config_id);
  const std::array<const char*, 2> values = {task_id_value.c_str(), config_id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  const bool validate_task_existence = path == GetPath::kTaskAware;
  const std::string& sql = validate_task_existence ? task_aware_get_sql_ : existing_task_get_sql_;
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
    if (validate_task_existence) {
      return core::protocol_errors::TaskNotFound(std::string(kTaskConfigNotFoundMessage));
    }
    if (path == GetPath::kDirect) {
      const auto task_configs_exist =
          PushTaskConfigCollectionExists(lease.value().get(), task_config_exists_sql_, task_id_value.c_str());
      if (!task_configs_exist.ok()) {
        return task_configs_exist.error();
      }
      if (!task_configs_exist.value()) {
        return core::protocol_errors::TaskNotFound(std::string(kTaskConfigNotFoundMessage));
      }
    }
    return core::Error::Validation(std::string(kConfigNotFoundMessage));
  }
  if (PQgetisnull(result.get(), 0, 0) != 0) {
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
  return ListInternal(task_id, page_size, page_token, true);
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> PostgresPushNotificationStore::ListForExistingTask(
    std::string_view task_id, int page_size, std::string_view page_token) const {
  return ListInternal(task_id, page_size, page_token, false);
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> PostgresPushNotificationStore::ListForTask(
    std::string_view task_id, int page_size, std::string_view page_token, const TaskStore& task_store) const {
  const auto validation = ValidatePushListRequest(task_id, page_size, page_token);
  if (!validation.ok()) {
    return validation.error();
  }
  const auto* postgres_task_store = dynamic_cast<const PostgresTaskStore*>(&task_store);
  if (postgres_task_store != nullptr && postgres_task_store->UsesExecutionIdentity(execution_identity_)) {
    return ListInternal(task_id, page_size, page_token, true);
  }
  const auto task = task_store.Get(task_id);
  if (!task.ok()) {
    return task.error();
  }
  return ListInternal(task_id, page_size, page_token, false);
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> PostgresPushNotificationStore::ListInternal(
    std::string_view task_id, int page_size, std::string_view page_token, bool validate_task_existence) const {
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
  const std::string& sql = validate_task_existence ? task_aware_list_sql_ : existing_task_list_sql_;
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
    if (validate_task_existence) {
      return core::protocol_errors::TaskNotFound(std::string(kTaskConfigNotFoundMessage));
    }
    return core::Error::Internal(std::string(kPushListMissingCountMessage));
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
  if (row_count < count.value() - offset.value()) {
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
  const std::string& sql = delete_sql_;
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
