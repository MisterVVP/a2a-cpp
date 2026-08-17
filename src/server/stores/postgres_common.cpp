// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/postgres_common.h"

#include <libpq-fe.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/server/stores/postgres_notification_store.h"
#include "a2a/server/stores/postgres_task_store.h"
#include "a2a/server/stores/sql_identifier.h"

namespace a2a::server::stores {
namespace {

constexpr std::size_t kAlterTaskRevisionSqlReserve = 70U;
constexpr std::size_t kTaskPushConfigLockFunctionSqlReserveSlack = 768U;
constexpr std::size_t kTaskPushConfigLockFunctionPrivilegeSqlReserveSlack = 64U;
constexpr std::size_t kTaskPushConfigMigrationMarkerSqlReserveSlack = 96U;
constexpr std::string_view kFeatureNotSupportedSqlState = "0A000";
constexpr std::string_view kInsufficientPrivilegeSqlState = "42501";
constexpr std::string_view kPostgresTaskRowLevelSecurityUnsupportedMessage =
    "PostgreSQL task-aware push configuration does not support row-level security on a2a_tasks";
constexpr std::string_view kPostgresPushTaskSelectRequiredMessage =
    "PostgreSQL push store role requires SELECT on a2a_tasks for task-aware creation";
constexpr auto kCurrentUserSql = std::to_array("SELECT current_user");
constinit const std::string kBeginSchemaTransactionSql = "BEGIN";
constinit const std::string kCommitSchemaTransactionSql = "COMMIT";
constinit const std::string kRollbackSchemaTransactionSql = "ROLLBACK";
constexpr std::string_view kBeginSchemaTransactionOperation = "begin postgres schema transaction";
constexpr std::string_view kCommitSchemaTransactionOperation = "commit postgres schema transaction";
constexpr std::string_view kRollbackSchemaTransactionOperation = "rollback postgres schema transaction";
constexpr std::string_view kPostgresErrorSeparator = ": ";
constexpr std::string_view kOpenPostgresConnectionOperation = "open postgres connection";
constexpr std::string_view kInitializePostgresSchemaOperation = "initialize postgres store schema";
constexpr std::string_view kReadPostgresEffectiveRoleOperation = "read postgres effective role";
constexpr std::string_view kReadPostgresEffectiveRoleMissingRowMessage =
    "read postgres effective role: query returned no row";
constexpr std::string_view kReadPostgresConnectionOptionsMissingMessage =
    "read postgres connection identity: unable to read libpq connection options";
constexpr std::string_view kConninfoTargetSessionAttributesKeyword = "target_session_attrs";

#ifdef A2A_POSTGRES_STORE_TESTING
std::mutex g_test_acquire_failure_mutex;
std::optional<core::Error> g_test_acquire_failure;
std::mutex g_test_connection_identity_mutex;
std::optional<PostgresExecutionIdentity> g_test_connection_identity_override;
thread_local PostgresOperationDiagnostics g_operation_diagnostics;
#endif

[[nodiscard]] std::string BuildPostgresErrorMessage(std::string_view operation, std::string_view message) {
  std::string error;
  error.reserve(operation.size() + kPostgresErrorSeparator.size() + message.size());
  error.append(operation);
  error.append(kPostgresErrorSeparator);
  error.append(message);
  return error;
}

[[nodiscard]] core::Error DatabaseError(PGconn* connection, std::string_view operation) {
  return core::Error::Internal(BuildPostgresErrorMessage(operation, PQerrorMessage(connection)));
}

[[nodiscard]] core::Error DatabaseResultError(PGresult* result, std::string_view operation) {
  return core::Error::Internal(BuildPostgresErrorMessage(operation, PQresultErrorMessage(result)));
}

[[nodiscard]] core::Error DatabaseConnectionError(std::string_view message) {
  return core::Error::Internal(BuildPostgresErrorMessage(kOpenPostgresConnectionOperation, message));
}

[[nodiscard]] std::string TaskCreatedSequence(std::string_view schema) {
  return QualifiedSqlIdentifier(schema, kTaskCreatedSequenceName);
}

[[nodiscard]] std::string PushCreatedSequence(std::string_view schema) {
  return QualifiedSqlIdentifier(schema, kPushCreatedSequenceName);
}

[[nodiscard]] std::string SqlStringLiteral(std::string_view value) {
  std::string literal;
  literal.reserve(value.size() + 2U);
  literal.push_back('\'');
  for (const char symbol : value) {
    if (symbol == '\'') {
      literal.append("''");
    } else {
      literal.push_back(symbol);
    }
  }
  literal.push_back('\'');
  return literal;
}

[[nodiscard]] std::string CreateIndexStatement(std::string_view index_name, const std::string& table_name,
                                               std::string_view columns) {
  const std::string quoted_index_name = QuoteSqlIdentifier(index_name);
  std::string statement;
  statement.reserve(std::string_view("CREATE INDEX IF NOT EXISTS ").size() + quoted_index_name.size() +
                    std::string_view(" ON ").size() + table_name.size() + 1 + columns.size() + 1);
  statement.append("CREATE INDEX IF NOT EXISTS ");
  statement.append(quoted_index_name);
  statement.append(" ON ");
  statement.append(table_name);
  statement.push_back(' ');
  statement.append(columns);
  statement.push_back(';');
  return statement;
}

[[nodiscard]] std::string BuildTaskPushConfigLockFunctionBody(std::string_view task_table) {
  const std::string task_table_literal = SqlStringLiteral(task_table);
  const std::string rls_error_code = SqlStringLiteral(kFeatureNotSupportedSqlState);
  const std::string rls_error_message = SqlStringLiteral(kPostgresTaskRowLevelSecurityUnsupportedMessage);
  const std::string privilege_error_code = SqlStringLiteral(kInsufficientPrivilegeSqlState);
  const std::string privilege_error_message = SqlStringLiteral(kPostgresPushTaskSelectRequiredMessage);
  std::string body;
  body.reserve(task_table.size() + (2U * task_table_literal.size()) + rls_error_code.size() + rls_error_message.size() +
               privilege_error_code.size() + privilege_error_message.size() +
               kTaskPushConfigLockFunctionSqlReserveSlack);
  body.append(
      "DECLARE caller_role name; BEGIN caller_role := NULLIF(pg_catalog.current_setting('role', true), 'none'); "
      "IF caller_role IS NULL THEN caller_role := session_user; END IF; IF EXISTS ("
      "SELECT 1 FROM pg_catalog.pg_class AS relation WHERE relation.oid = pg_catalog.to_regclass(");
  body.append(task_table_literal);
  body.append(") AND relation.relrowsecurity) THEN RAISE EXCEPTION USING ERRCODE = ");
  body.append(rls_error_code);
  body.append(", MESSAGE = ");
  body.append(rls_error_message);
  body.append("; END IF; IF NOT pg_catalog.has_table_privilege(caller_role, ");
  body.append(task_table_literal);
  body.append(", 'SELECT') THEN RAISE EXCEPTION USING ERRCODE = ");
  body.append(privilege_error_code);
  body.append(", MESSAGE = ");
  body.append(privilege_error_message);
  body.append("; END IF; PERFORM 1 FROM ");
  body.append(task_table);
  body.append(" WHERE id = requested_task_id FOR KEY SHARE; RETURN FOUND; END");
  return body;
}

[[nodiscard]] std::string BuildTaskPushConfigLockFunctionSql(std::string_view function, std::string_view task_table) {
  const std::string body = BuildTaskPushConfigLockFunctionBody(task_table);
  std::string sql;
  sql.reserve(function.size() + body.size() + kTaskPushConfigLockFunctionSqlReserveSlack);
  sql.append("CREATE OR REPLACE FUNCTION ");
  sql.append(function);
  sql.append(
      "(requested_task_id text) RETURNS boolean LANGUAGE plpgsql VOLATILE SECURITY DEFINER "
      "SET search_path = pg_catalog AS $a2a$ ");
  sql.append(body);
  sql.append(" $a2a$;");
  return sql;
}

[[nodiscard]] std::string BuildRevokeTaskPushConfigLockFunctionSql(std::string_view function) {
  std::string sql;
  sql.reserve(function.size() + kTaskPushConfigLockFunctionPrivilegeSqlReserveSlack);
  sql.append("REVOKE ALL ON FUNCTION ");
  sql.append(function);
  sql.append("(text) FROM PUBLIC;");
  return sql;
}

[[nodiscard]] std::string BuildGrantTaskPushConfigLockFunctionSql(std::string_view function) {
  std::string sql;
  sql.reserve(function.size() + kTaskPushConfigLockFunctionPrivilegeSqlReserveSlack);
  sql.append("GRANT EXECUTE ON FUNCTION ");
  sql.append(function);
  sql.append("(text) TO CURRENT_USER;");
  return sql;
}

[[nodiscard]] std::string BuildTaskPushConfigMigrationMarkerSql(std::string_view function) {
  std::string sql;
  sql.reserve(function.size() + kTaskPushConfigMigrationMarkerSqlReserveSlack);
  sql.append("COMMENT ON FUNCTION ");
  sql.append(function);
  sql.append("(text) IS ");
  sql.append(SqlStringLiteral(kTaskPushConfigMigrationId));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildDeleteTaskPushConfigsFunctionBody(std::string_view push_table) {
  std::string body;
  body.reserve(push_table.size() + kDeleteTaskPushConfigsFunctionSqlReserveSlack);
  body.append("BEGIN DELETE FROM ");
  body.append(push_table);
  body.append(" WHERE task_id = OLD.id AND local_postgres_task; RETURN OLD; END");
  return body;
}

[[nodiscard]] std::string BuildDeleteTaskPushConfigsFunctionSql(std::string_view function,
                                                                std::string_view push_table) {
  const std::string body = BuildDeleteTaskPushConfigsFunctionBody(push_table);
  std::string sql;
  sql.reserve(function.size() + body.size() + kDeleteTaskPushConfigsFunctionSqlReserveSlack);
  sql.append("CREATE OR REPLACE FUNCTION ");
  sql.append(function);
  sql.append("() RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog AS $a2a$ ");
  sql.append(body);
  sql.append(" $a2a$;");
  return sql;
}

[[nodiscard]] std::string BuildRevokeDeleteTaskPushConfigsFunctionSql(std::string_view function) {
  std::string sql;
  sql.reserve(function.size() + kRevokeDeleteTaskPushConfigsFunctionSqlReserveSlack);
  sql.append("REVOKE ALL ON FUNCTION ");
  sql.append(function);
  sql.append("() FROM PUBLIC;");
  return sql;
}

[[nodiscard]] std::string BuildDeleteTaskPushConfigsMigrationMarkerSql(std::string_view function) {
  std::string sql;
  sql.reserve(function.size() + kTaskPushConfigMigrationMarkerSqlReserveSlack);
  sql.append("COMMENT ON FUNCTION ");
  sql.append(function);
  sql.append("() IS ");
  sql.append(SqlStringLiteral(kTaskPushConfigMigrationId));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string LibpqValue(const char* value) {
  return value == nullptr ? std::string{} : std::string(value);
}

struct ConninfoDeleter final {
  void operator()(PQconninfoOption* options) const noexcept { PQconninfoFree(options); }
};

using Conninfo = std::unique_ptr<PQconninfoOption, ConninfoDeleter>;

[[nodiscard]] std::string ConninfoValue(PQconninfoOption* options, std::string_view keyword) {
  for (PQconninfoOption* option = options; option->keyword != nullptr; ++option) {
    if (keyword == option->keyword && option->val != nullptr) {
      return option->val;
    }
  }
  return {};
}

[[nodiscard]] core::Result<PostgresExecutionIdentity> ReadPostgresExecutionIdentity(PGconn* connection) {
  Conninfo options(PQconninfo(connection));
  if (options == nullptr) {
    return core::Error::Internal(std::string(kReadPostgresConnectionOptionsMissingMessage));
  }

  PostgresExecutionIdentity identity;
  identity.storage.host = LibpqValue(PQhost(connection));
  identity.storage.host_address = LibpqValue(PQhostaddr(connection));
  identity.storage.port = LibpqValue(PQport(connection));
  identity.storage.database = LibpqValue(PQdb(connection));
  identity.storage.target_session_attributes = ConninfoValue(options.get(), kConninfoTargetSessionAttributesKeyword);
  PgResult role_result(PQexec(connection, kCurrentUserSql.data()));
  const auto role_checked = CheckTuples(connection, role_result.get(), kReadPostgresEffectiveRoleOperation);
  if (!role_checked.ok()) {
    return role_checked.error();
  }
  if (PQntuples(role_result.get()) != 1) {
    return core::Error::Internal(std::string(kReadPostgresEffectiveRoleMissingRowMessage));
  }
  identity.effective_role = LibpqValue(PQgetvalue(role_result.get(), 0, 0));
  return identity;
}

[[nodiscard]] core::Result<void> ExecuteSchemaStatements(PGconn* connection,
                                                         const std::vector<std::string>& statements) {
  const auto begun = Exec(connection, kBeginSchemaTransactionSql, kBeginSchemaTransactionOperation);
  if (!begun.ok()) {
    return begun.error();
  }
  for (const auto& statement : statements) {
    const auto executed = Exec(connection, statement, kInitializePostgresSchemaOperation);
    if (!executed.ok()) {
      (void)Exec(connection, kRollbackSchemaTransactionSql, kRollbackSchemaTransactionOperation);
      return executed.error();
    }
  }
  auto committed = Exec(connection, kCommitSchemaTransactionSql, kCommitSchemaTransactionOperation);
  if (!committed.ok()) {
    (void)Exec(connection, kRollbackSchemaTransactionSql, kRollbackSchemaTransactionOperation);
  }
  return committed;
}

[[nodiscard]] std::string BuildDeleteTaskPushConfigsTriggerSql(std::string_view function, std::string_view task_table) {
  const std::string trigger = QuoteSqlIdentifier(kDeleteTaskPushConfigsTrigger);
  std::string sql;
  sql.reserve((2U * trigger.size()) + function.size() + (2U * task_table.size()) +
              kDeleteTaskPushConfigsTriggerSqlReserveSlack);
  sql.append("DROP TRIGGER IF EXISTS ");
  sql.append(trigger);
  sql.append(" ON ");
  sql.append(task_table);
  sql.append("; CREATE TRIGGER ");
  sql.append(trigger);
  sql.append(" AFTER DELETE ON ");
  sql.append(task_table);
  sql.append(" FOR EACH ROW EXECUTE FUNCTION ");
  sql.append(function);
  sql.append("();");
  return sql;
}

#ifdef A2A_POSTGRES_STORE_TESTING
[[nodiscard]] std::optional<core::Error> ConsumePostgresAcquireFailureForTesting() {
  std::lock_guard<std::mutex> lock(g_test_acquire_failure_mutex);
  if (!g_test_acquire_failure.has_value()) {
    return std::nullopt;
  }
  auto error = std::move(g_test_acquire_failure);
  g_test_acquire_failure.reset();
  return error;
}

[[nodiscard]] std::optional<PostgresExecutionIdentity> ConsumePostgresConnectionIdentityOverrideForTesting() {
  std::lock_guard<std::mutex> lock(g_test_connection_identity_mutex);
  auto identity = std::move(g_test_connection_identity_override);
  g_test_connection_identity_override.reset();
  return identity;
}
#endif

[[nodiscard]] core::Result<void> ValidatePostgresConnectionIdentity(
    PGconn* connection, const PostgresExecutionIdentity& expected_identity) {
#ifdef A2A_POSTGRES_STORE_TESTING
  if (auto overridden_identity = ConsumePostgresConnectionIdentityOverrideForTesting();
      overridden_identity.has_value()) {
    if (overridden_identity.value() != expected_identity) {
      return core::Error::Internal(std::string(kPostgresConnectionPoolIdentityMismatchMessage));
    }
    return {};
  }
#endif
  const auto actual_identity = ReadPostgresExecutionIdentity(connection);
  if (!actual_identity.ok()) {
    return actual_identity.error();
  }
  if (actual_identity.value() != expected_identity) {
    return core::Error::Internal(std::string(kPostgresConnectionPoolIdentityMismatchMessage));
  }
  return {};
}

}  // namespace

void PgResultDeleter::operator()(PGresult* result) const noexcept { PQclear(result); }

void PgConnectionDeleter::operator()(PGconn* connection) const noexcept { PQfinish(connection); }

std::string TaskTable(std::string_view schema) { return QualifiedSqlIdentifier(schema, kTaskTableName); }

std::string PushTable(std::string_view schema) { return QualifiedSqlIdentifier(schema, kPushTableName); }

std::string TaskPushConfigLockFunction(std::string_view schema) {
  return QualifiedSqlIdentifier(schema, kTaskPushConfigLockFunction);
}

std::string ExpectedTaskPushConfigLockFunctionBody(std::string_view schema) {
  return BuildTaskPushConfigLockFunctionBody(TaskTable(schema));
}

std::string ExpectedDeleteTaskPushConfigsFunctionBody(std::string_view schema) {
  return BuildDeleteTaskPushConfigsFunctionBody(PushTable(schema));
}

PostgresStorageAuthority ClassifyPostgresStorageAuthority(const PostgresStorageIdentity& lhs,
                                                          const PostgresStorageIdentity& rhs) noexcept {
  if (lhs.database != rhs.database || lhs.schema != rhs.schema) {
    return PostgresStorageAuthority::kExternal;
  }
  const bool lhs_has_authority_id = !lhs.storage_authority_id.empty();
  const bool rhs_has_authority_id = !rhs.storage_authority_id.empty();
  if (lhs_has_authority_id || rhs_has_authority_id) {
    if (!lhs_has_authority_id || !rhs_has_authority_id) {
      return PostgresStorageAuthority::kUncertain;
    }
    return lhs.storage_authority_id == rhs.storage_authority_id ? PostgresStorageAuthority::kLocal
                                                                : PostgresStorageAuthority::kExternal;
  }
  if (lhs == rhs) {
    return PostgresStorageAuthority::kLocal;
  }
  // Endpoint differences can be aliases or failover candidates, so they do not
  // prove external ownership without an explicit authority identifier.
  return PostgresStorageAuthority::kUncertain;
}

core::Result<void> ValidatePostgresStoreOptions(const PostgresStoreOptions& options) {
  if (options.connection_pool_size == 0U) {
    return core::Error::Validation(std::string(kPostgresConnectionPoolSizeValidationMessage));
  }
  if (!IsValidSqlIdentifier(options.schema)) {
    return core::Error::Validation("PostgreSQL schema must be a simple SQL identifier");
  }
  if (options.schema.size() > kPostgresIdentifierMaxBytes) {
    return core::Error::Validation("PostgreSQL schema must be at most 63 bytes");
  }
  if (options.connection_string.empty()) {
    return core::Error::Validation("Postgres connection_string is required");
  }
  return {};
}

void ValidatePostgresStoreOptionsOrThrow(const PostgresStoreOptions& options) {
  const auto validation = ValidatePostgresStoreOptions(options);
  if (!validation.ok()) {
    throw std::invalid_argument(std::string(validation.error().message()));
  }
}

core::Result<void> CheckCommand(PGconn* connection, PGresult* result, std::string_view operation) {
  if (result == nullptr) {
    return DatabaseError(connection, operation);
  }
  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    return DatabaseResultError(result, operation);
  }
  return {};
}

core::Result<void> CheckTuples(PGconn* connection, PGresult* result, std::string_view operation) {
  if (result == nullptr) {
    return DatabaseError(connection, operation);
  }
  if (PQresultStatus(result) != PGRES_TUPLES_OK) {
    return DatabaseResultError(result, operation);
  }
  return {};
}

core::Result<void> Exec(PGconn* connection, const std::string& sql, std::string_view operation) {
  PgResult result(PQexec(connection, sql.c_str()));
  return CheckCommand(connection, result.get(), operation);
}

Transaction::Transaction(PGconn* connection) : connection_(connection) {}

core::Result<void> Transaction::Begin() {
#ifdef A2A_POSTGRES_STORE_TESTING
  const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTransactionBegin);
#endif
  return Exec(connection_, "BEGIN", "begin postgres transaction");
}

core::Result<void> Transaction::Commit() {
#ifdef A2A_POSTGRES_STORE_TESTING
  const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTransactionCommit);
#endif
  const auto committed = Exec(connection_, "COMMIT", "commit postgres transaction");
  if (committed.ok()) {
    committed_ = true;
  }
  return committed;
}

Transaction::~Transaction() {
  if (!committed_ && connection_ != nullptr) {
    PgResult rollback(PQexec(connection_, "ROLLBACK"));
  }
}

PostgresConnectionPool::PostgresConnectionPool(std::string connection_string, std::size_t size)
    : connection_string_(std::move(connection_string)), capacity_(size) {
  if (size == 0U) {
    throw std::invalid_argument(std::string(kPostgresConnectionPoolSizeValidationMessage));
  }
  connections_.reserve(size);
  auto first_connection = OpenConnection();
  if (!first_connection.ok()) {
    throw std::runtime_error(std::string(first_connection.error().message()));
  }
  auto identity = ReadPostgresExecutionIdentity(first_connection.value().get());
  if (!identity.ok()) {
    throw std::runtime_error(std::string(identity.error().message()));
  }
  database_identity_ = std::move(identity.value());
  connections_.push_back(std::move(first_connection.value()));
  while (connections_.size() < size) {
    auto connection = OpenConnection();
    if (!connection.ok()) {
      throw std::runtime_error(std::string(connection.error().message()));
    }
    const auto identity_checked = ValidatePostgresConnectionIdentity(connection.value().get(), database_identity_);
    if (!identity_checked.ok()) {
      throw std::runtime_error(std::string(identity_checked.error().message()));
    }
    connections_.push_back(std::move(connection.value()));
  }
}

PostgresConnectionPool::Lease::Lease(PostgresConnectionPool* pool, PgConnection connection)
    : pool_(pool), connection_(std::move(connection)) {}

PostgresConnectionPool::Lease::Lease(Lease&& other) noexcept
    : pool_(std::exchange(other.pool_, nullptr)), connection_(std::move(other.connection_)) {}

PostgresConnectionPool::Lease::~Lease() {
  if (pool_ != nullptr && connection_ != nullptr) {
    pool_->Return(std::move(connection_));
  }
}

PGconn* PostgresConnectionPool::Lease::get() const noexcept { return connection_.get(); }

core::Result<PostgresConnectionPool::Lease> PostgresConnectionPool::Acquire() {
#ifdef A2A_POSTGRES_STORE_TESTING
  if (auto failure = ConsumePostgresAcquireFailureForTesting(); failure.has_value()) {
    return std::move(*failure);
  }
#endif

  PgConnection connection;
  {
#ifdef A2A_POSTGRES_STORE_TESTING
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kConnectionAcquireWait);
#endif
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] { return !connections_.empty(); });
    connection = std::move(connections_.back());
    connections_.pop_back();
  }

  if (PQstatus(connection.get()) != CONNECTION_OK) {
    auto reopened = OpenConnection();
    if (!reopened.ok()) {
      Return(std::move(connection));
      return reopened.error();
    }
    const auto identity_checked = ValidatePostgresConnectionIdentity(reopened.value().get(), database_identity_);
    if (!identity_checked.ok()) {
      Return(std::move(connection));
      return identity_checked.error();
    }
    connection = std::move(reopened.value());
  }
  return Lease(this, std::move(connection));
}

std::size_t PostgresConnectionPool::capacity() const noexcept { return capacity_; }

PostgresStorageCoordinates PostgresConnectionPool::StorageCoordinates(std::string schema,
                                                                      std::string_view storage_authority_id) const {
  PostgresStorageCoordinates identity = database_identity_.storage;
  identity.schema = std::move(schema);
  identity.storage_authority_id = storage_authority_id;
  return identity;
}

PostgresExecutionIdentity PostgresConnectionPool::ExecutionIdentity(std::string schema,
                                                                    std::string_view storage_authority_id) const {
  PostgresExecutionIdentity identity = database_identity_;
  identity.storage.schema = std::move(schema);
  identity.storage.storage_authority_id = storage_authority_id;
  return identity;
}

core::Result<PgConnection> PostgresConnectionPool::OpenConnection() const {
  PgConnection connection(PQconnectdb(connection_string_.c_str()));
  if (connection == nullptr || PQstatus(connection.get()) != CONNECTION_OK) {
    const std::string message =
        connection == nullptr ? "unable to allocate PostgreSQL connection" : PQerrorMessage(connection.get());
    return DatabaseConnectionError(message);
  }
  return connection;
}

void PostgresConnectionPool::Return(PgConnection connection) {
  std::lock_guard<std::mutex> lock(mutex_);
  connections_.push_back(std::move(connection));
  condition_.notify_one();
}

core::Result<void> InitializeSchema(PGconn* connection, const PostgresStoreOptions& options) {
  const auto validation = ValidatePostgresStoreOptions(options);
  if (!validation.ok()) {
    return validation.error();
  }
  if (options.auto_create_schema && options.schema != kPublicSchema) {
    const std::string create_schema = "CREATE SCHEMA IF NOT EXISTS " + QuoteSqlIdentifier(options.schema);
    const auto created = Exec(connection, create_schema, "create postgres schema");
    if (!created.ok()) {
      return created.error();
    }
  }
  if (!options.auto_create_schema) {
    return {};
  }

  const std::string tasks = TaskTable(options.schema);
  const std::string push_configs = PushTable(options.schema);
  const std::string task_created_sequence = TaskCreatedSequence(options.schema);
  const std::string push_created_sequence = PushCreatedSequence(options.schema);
  const std::string delete_task_push_configs_function =
      QualifiedSqlIdentifier(options.schema, kDeleteTaskPushConfigsFunction);
  const std::string task_push_config_lock_function = TaskPushConfigLockFunction(options.schema);
  const std::string task_created_sequence_regclass = SqlStringLiteral(task_created_sequence);
  const std::string push_created_sequence_regclass = SqlStringLiteral(push_created_sequence);
  const std::string create_task_created_sequence = "CREATE SEQUENCE IF NOT EXISTS " + task_created_sequence + ";";
  const std::string create_push_created_sequence = "CREATE SEQUENCE IF NOT EXISTS " + push_created_sequence + ";";
  const std::string create_tasks = "CREATE TABLE IF NOT EXISTS " + tasks +
                                   " (id TEXT PRIMARY KEY, context_id TEXT NOT NULL, state INTEGER NOT NULL, "
                                   "has_status_timestamp BOOLEAN NOT NULL DEFAULT FALSE, "
                                   "status_seconds BIGINT NOT NULL DEFAULT 0, status_nanos INTEGER NOT NULL DEFAULT 0, "
                                   "revision BIGINT NOT NULL DEFAULT 1, "
                                   "created_sequence BIGINT NOT NULL DEFAULT nextval(" +
                                   task_created_sequence_regclass +
                                   "), "
                                   "task_proto BYTEA NOT NULL, updated_at TIMESTAMPTZ NOT NULL DEFAULT now());";
  const std::string add_tasks_created_sequence =
      "ALTER TABLE " + tasks + " ADD COLUMN IF NOT EXISTS created_sequence BIGINT NOT NULL DEFAULT nextval(" +
      task_created_sequence_regclass + ");";
  const std::string add_tasks_has_status_timestamp =
      "ALTER TABLE " + tasks + " ADD COLUMN IF NOT EXISTS has_status_timestamp BOOLEAN NOT NULL DEFAULT FALSE;";
  std::string add_tasks_revision = "ALTER TABLE ";
  add_tasks_revision.reserve(add_tasks_revision.size() + tasks.size() + kAlterTaskRevisionSqlReserve);
  add_tasks_revision.append(tasks);
  add_tasks_revision.append(" ADD COLUMN IF NOT EXISTS revision BIGINT NOT NULL DEFAULT 1;");
  const std::string create_tasks_created_sequence_index =
      CreateIndexStatement(kTasksCreatedSequenceIndex, tasks, kTasksCreatedSequenceIndexColumns);
  const std::string create_tasks_context_index =
      CreateIndexStatement(kTasksContextIndex, tasks, kTasksContextIndexColumns);
  const std::string create_tasks_state_index = CreateIndexStatement(kTasksStateIndex, tasks, kTasksStateIndexColumns);
  std::string create_push_configs = "CREATE TABLE IF NOT EXISTS ";
  create_push_configs.append(push_configs);
  create_push_configs.append(
      " (task_id TEXT NOT NULL, config_id TEXT NOT NULL, url TEXT NOT NULL, "
      "created_sequence BIGINT NOT NULL DEFAULT nextval(");
  create_push_configs.append(push_created_sequence_regclass);
  create_push_configs.append(
      "), config_proto BYTEA NOT NULL, local_postgres_task BOOLEAN NOT NULL DEFAULT FALSE, "
      "updated_at TIMESTAMPTZ NOT NULL DEFAULT now(), PRIMARY KEY (task_id, config_id));");
  std::string add_push_config_provenance = "ALTER TABLE ";
  add_push_config_provenance.append(push_configs);
  add_push_config_provenance.append(" ADD COLUMN IF NOT EXISTS local_postgres_task BOOLEAN NOT NULL DEFAULT FALSE;");
  std::string remove_push_configs_task_foreign_key = "ALTER TABLE ";
  remove_push_configs_task_foreign_key.append(push_configs);
  remove_push_configs_task_foreign_key.append(" DROP CONSTRAINT IF EXISTS ");
  remove_push_configs_task_foreign_key.append(QuoteSqlIdentifier(kPushConfigsTaskForeignKey));
  remove_push_configs_task_foreign_key.push_back(';');
  const std::string create_task_push_config_lock_function =
      BuildTaskPushConfigLockFunctionSql(task_push_config_lock_function, tasks);
  const std::string revoke_task_push_config_lock_function =
      BuildRevokeTaskPushConfigLockFunctionSql(task_push_config_lock_function);
  const std::string grant_task_push_config_lock_function =
      BuildGrantTaskPushConfigLockFunctionSql(task_push_config_lock_function);
  const std::string mark_task_push_config_migration =
      BuildTaskPushConfigMigrationMarkerSql(task_push_config_lock_function);
  const std::string create_delete_task_push_configs_function =
      BuildDeleteTaskPushConfigsFunctionSql(delete_task_push_configs_function, push_configs);
  const std::string create_delete_task_push_configs_trigger =
      BuildDeleteTaskPushConfigsTriggerSql(delete_task_push_configs_function, tasks);
  const std::string revoke_delete_task_push_configs_function =
      BuildRevokeDeleteTaskPushConfigsFunctionSql(delete_task_push_configs_function);
  const std::string mark_delete_task_push_configs_migration =
      BuildDeleteTaskPushConfigsMigrationMarkerSql(delete_task_push_configs_function);
  const std::string add_push_configs_created_sequence =
      "ALTER TABLE " + push_configs + " ADD COLUMN IF NOT EXISTS created_sequence BIGINT NOT NULL DEFAULT nextval(" +
      push_created_sequence_regclass + ");";
  const std::string create_push_configs_task_index =
      CreateIndexStatement(kPushConfigsTaskIndex, push_configs, kPushConfigsTaskIndexColumns);
  const std::string create_push_configs_created_sequence_index =
      CreateIndexStatement(kPushConfigsCreatedSequenceIndex, push_configs, kPushConfigsCreatedSequenceIndexColumns);

  const std::vector<std::string> schema_statements = {create_task_created_sequence,
                                                      create_push_created_sequence,
                                                      create_tasks,
                                                      add_tasks_created_sequence,
                                                      add_tasks_has_status_timestamp,
                                                      add_tasks_revision,
                                                      create_task_push_config_lock_function,
                                                      revoke_task_push_config_lock_function,
                                                      grant_task_push_config_lock_function,
                                                      create_tasks_created_sequence_index,
                                                      create_tasks_context_index,
                                                      create_tasks_state_index,
                                                      create_push_configs,
                                                      add_push_config_provenance,
                                                      remove_push_configs_task_foreign_key,
                                                      create_delete_task_push_configs_function,
                                                      revoke_delete_task_push_configs_function,
                                                      create_delete_task_push_configs_trigger,
                                                      add_push_configs_created_sequence,
                                                      create_push_configs_task_index,
                                                      create_push_configs_created_sequence_index,
                                                      mark_delete_task_push_configs_migration,
                                                      mark_task_push_config_migration};
  return ExecuteSchemaStatements(connection, schema_statements);
}

std::shared_ptr<PostgresConnectionPool> MakePool(const PostgresStoreOptions& options) {
  ValidatePostgresStoreOptionsOrThrow(options);
  return std::make_shared<PostgresConnectionPool>(options.connection_string, options.connection_pool_size);
}

PostgresConnectionPool::Lease AcquireOrThrow(PostgresConnectionPool& pool) {
  auto lease = pool.Acquire();
  if (!lease.ok()) {
    throw std::runtime_error(std::string(lease.error().message()));
  }
  return std::move(lease.value());
}

#ifdef A2A_POSTGRES_STORE_TESTING
void FailNextPostgresAcquireForTesting(core::Error error) {
  std::lock_guard<std::mutex> lock(g_test_acquire_failure_mutex);
  g_test_acquire_failure = std::move(error);
}

void OverrideNextPostgresConnectionIdentityForTesting(PostgresExecutionIdentity identity) {
  std::lock_guard<std::mutex> lock(g_test_connection_identity_mutex);
  g_test_connection_identity_override = std::move(identity);
}

void ClearPostgresConnectionIdentityOverrideForTesting() {
  std::lock_guard<std::mutex> lock(g_test_connection_identity_mutex);
  g_test_connection_identity_override.reset();
}

void ResetPostgresOperationDiagnosticsForTesting() noexcept { g_operation_diagnostics = {}; }

PostgresDiagnosticTimerForTesting::PostgresDiagnosticTimerForTesting(PostgresDiagnosticPhase phase) noexcept
    : phase_(phase), started_(std::chrono::steady_clock::now()) {}

PostgresDiagnosticTimerForTesting::~PostgresDiagnosticTimerForTesting() {
  const auto elapsed = std::chrono::steady_clock::now() - started_;
  g_operation_diagnostics.elapsed_ms[static_cast<std::size_t>(phase_)] +=
      std::chrono::duration<double, std::milli>(elapsed).count();
  ++g_operation_diagnostics.call_count[static_cast<std::size_t>(phase_)];
}

PostgresOperationDiagnostics TakePostgresOperationDiagnosticsForTesting() noexcept {
  PostgresOperationDiagnostics diagnostics = g_operation_diagnostics;
  g_operation_diagnostics = {};
  return diagnostics;
}
#endif

PostgresStoreFactory::PostgresStoreFactory(PostgresStoreOptions options) : options_(std::move(options)) {}

StoreBackendKind PostgresStoreFactory::backend_kind() const noexcept { return StoreBackendKind::kPostgres; }

core::Result<std::unique_ptr<TaskStore>> PostgresStoreFactory::CreateTaskStore() const {
  const auto validation = ValidatePostgresStoreOptions(options_);
  if (!validation.ok()) {
    return validation.error();
  }
  try {
    return std::unique_ptr<TaskStore>(std::make_unique<PostgresTaskStore>(options_));
  } catch (const std::exception& ex) {
    return core::Error::Internal(std::string("failed to create PostgreSQL task store: ") + ex.what());
  }
}

core::Result<std::unique_ptr<PushNotificationStore>> PostgresStoreFactory::CreatePushNotificationStore() const {
  const auto validation = ValidatePostgresStoreOptions(options_);
  if (!validation.ok()) {
    return validation.error();
  }
  try {
    return std::unique_ptr<PushNotificationStore>(std::make_unique<PostgresPushNotificationStore>(options_));
  } catch (const std::exception& ex) {
    return core::Error::Internal(std::string("failed to create PostgreSQL push notification store: ") + ex.what());
  }
}

core::Result<StoreBundle> PostgresStoreFactory::CreateStoreBundle() const {
  const auto validation = ValidatePostgresStoreOptions(options_);
  if (!validation.ok()) {
    return validation.error();
  }
  try {
    auto pool = MakePool(options_);
    StoreBundle bundle;
    bundle.task_store = std::make_unique<PostgresTaskStore>(pool, options_);
    bundle.push_store = std::make_unique<PostgresPushNotificationStore>(std::move(pool), options_);
    return bundle;
  } catch (const std::exception& ex) {
    return core::Error::Internal(std::string("failed to create PostgreSQL store bundle: ") + ex.what());
  }
}

const PostgresStoreOptions& PostgresStoreFactory::options() const noexcept { return options_; }

}  // namespace a2a::server::stores
