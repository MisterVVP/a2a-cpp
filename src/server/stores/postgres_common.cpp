// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/postgres_common.h"

#include <libpq-fe.h>

#include <chrono>
#include <condition_variable>
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

#ifdef A2A_POSTGRES_STORE_TESTING
std::mutex g_test_acquire_failure_mutex;
std::optional<core::Error> g_test_acquire_failure;
thread_local PostgresOperationDiagnostics g_operation_diagnostics;
#endif

[[nodiscard]] core::Error DatabaseError(PGconn* connection, std::string_view operation) {
  return core::Error::Internal(std::string(operation) + ": " + PQerrorMessage(connection));
}

[[nodiscard]] core::Error DatabaseResultError(PGresult* result, std::string_view operation) {
  return core::Error::Internal(std::string(operation) + ": " + PQresultErrorMessage(result));
}

[[nodiscard]] core::Error DatabaseConnectionError(std::string_view message) {
  return core::Error::Internal("open postgres connection: " + std::string(message));
}

[[nodiscard]] std::string TaskCreatedSequence(std::string_view schema) {
  return QualifiedSqlIdentifier(schema, kTaskCreatedSequenceName);
}

[[nodiscard]] std::string PushCreatedSequence(std::string_view schema) {
  return QualifiedSqlIdentifier(schema, kPushCreatedSequenceName);
}

[[nodiscard]] std::string SqlStringLiteral(const std::string& value) {
  std::string literal = "'";
  for (const char symbol : value) {
    literal += symbol == '\'' ? "''" : std::string(1, symbol);
  }
  literal += "'";
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
#endif

}  // namespace

void PgResultDeleter::operator()(PGresult* result) const noexcept { PQclear(result); }

void PgConnectionDeleter::operator()(PGconn* connection) const noexcept { PQfinish(connection); }

std::string TaskTable(std::string_view schema) { return QualifiedSqlIdentifier(schema, kTaskTableName); }

std::string PushTable(std::string_view schema) { return QualifiedSqlIdentifier(schema, kPushTableName); }

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
  for (std::size_t index = 0; index < size; ++index) {
    auto connection = OpenConnection();
    if (!connection.ok()) {
      throw std::runtime_error(std::string(connection.error().message()));
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
    connection = std::move(reopened.value());
  }
  return Lease(this, std::move(connection));
}

std::size_t PostgresConnectionPool::capacity() const noexcept { return capacity_; }

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
  const std::string create_push_configs = "CREATE TABLE IF NOT EXISTS " + push_configs +
                                          " (task_id TEXT NOT NULL, config_id TEXT NOT NULL, url TEXT NOT NULL, "
                                          "created_sequence BIGINT NOT NULL DEFAULT nextval(" +
                                          push_created_sequence_regclass +
                                          "), "
                                          "config_proto BYTEA NOT NULL, updated_at TIMESTAMPTZ NOT NULL DEFAULT now(), "
                                          "PRIMARY KEY (task_id, config_id));";
  const std::string add_push_configs_task_foreign_key =
      "DO $a2a$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_constraint WHERE conrelid = '" + push_configs +
      "'::regclass AND conname = 'a2a_push_configs_task_fk') THEN ALTER TABLE " + push_configs +
      " ADD CONSTRAINT a2a_push_configs_task_fk FOREIGN KEY (task_id) REFERENCES " + tasks +
      " (id) ON DELETE CASCADE; END IF; END $a2a$;";
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
                                                      create_tasks_created_sequence_index,
                                                      create_tasks_context_index,
                                                      create_tasks_state_index,
                                                      create_push_configs,
                                                      add_push_configs_task_foreign_key,
                                                      add_push_configs_created_sequence,
                                                      create_push_configs_task_index,
                                                      create_push_configs_created_sequence_index};
  for (const auto& statement : schema_statements) {
    const auto executed = Exec(connection, statement, "initialize postgres store schema");
    if (!executed.ok()) {
      return executed.error();
    }
  }
  return {};
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
