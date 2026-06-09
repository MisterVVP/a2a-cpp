// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/postgres_store.h"

#include <libpq-fe.h>

#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"
#include "a2a/server/sql_identifier.h"

namespace a2a::server::stores {
namespace {

constexpr std::string_view kPublicSchema = "public";
constexpr std::string_view kTaskIdRequiredMessage = "Task id is required";
constexpr std::string_view kTaskNotFoundMessage = "Task not found";
constexpr std::string_view kTaskIdFieldRequiredMessage = "Task.id is required";
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

#ifdef A2A_POSTGRES_STORE_TESTING
std::mutex g_test_acquire_failure_mutex;
std::optional<core::Error> g_test_acquire_failure;
#endif

struct PgResultDeleter final {
  void operator()(PGresult* result) const noexcept { PQclear(result); }
};

using PgResult = std::unique_ptr<PGresult, PgResultDeleter>;

struct PgConnectionDeleter final {
  void operator()(PGconn* connection) const noexcept { PQfinish(connection); }
};

using PgConnection = std::unique_ptr<PGconn, PgConnectionDeleter>;

[[nodiscard]] core::Error DatabaseError(PGconn* connection, std::string_view operation) {
  return core::Error::Internal(std::string(operation) + ": " + PQerrorMessage(connection));
}

[[nodiscard]] core::Error DatabaseResultError(PGresult* result, std::string_view operation) {
  return core::Error::Internal(std::string(operation) + ": " + PQresultErrorMessage(result));
}

[[nodiscard]] core::Error DatabaseConnectionError(std::string_view message) {
  return core::Error::Internal("open postgres connection: " + std::string(message));
}

[[nodiscard]] std::string TaskTable(std::string_view schema) { return QualifiedSqlIdentifier(schema, "a2a_tasks"); }
[[nodiscard]] std::string PushTable(std::string_view schema) {
  return QualifiedSqlIdentifier(schema, "a2a_push_notification_configs");
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

[[nodiscard]] core::Result<void> ValidatePostgresStoreOptions(const PostgresStoreOptions& options) {
  if (!IsValidSqlIdentifier(options.schema)) {
    return core::Error::Validation("PostgreSQL schema must be a simple SQL identifier");
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

[[nodiscard]] core::Result<void> CheckCommand(PGconn* connection, PGresult* result, std::string_view operation) {
  if (result == nullptr) {
    return DatabaseError(connection, operation);
  }
  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    return DatabaseResultError(result, operation);
  }
  return {};
}

[[nodiscard]] core::Result<void> CheckTuples(PGconn* connection, PGresult* result, std::string_view operation) {
  if (result == nullptr) {
    return DatabaseError(connection, operation);
  }
  if (PQresultStatus(result) != PGRES_TUPLES_OK) {
    return DatabaseResultError(result, operation);
  }
  return {};
}

[[nodiscard]] core::Result<void> Exec(PGconn* connection, const std::string& sql, std::string_view operation) {
  PgResult result(PQexec(connection, sql.c_str()));
  return CheckCommand(connection, result.get(), operation);
}

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

class Transaction final {
 public:
  explicit Transaction(PGconn* connection) : connection_(connection) {}
  [[nodiscard]] core::Result<void> Begin() { return Exec(connection_, "BEGIN", "begin postgres transaction"); }
  [[nodiscard]] core::Result<void> Commit() {
    committed_ = true;
    return Exec(connection_, "COMMIT", "commit postgres transaction");
  }
  ~Transaction() {
    if (!committed_ && connection_ != nullptr) {
      PgResult rollback(PQexec(connection_, "ROLLBACK"));
    }
  }

 private:
  PGconn* connection_ = nullptr;
  bool committed_ = false;
};

}  // namespace

#ifdef A2A_POSTGRES_STORE_TESTING
namespace {
[[nodiscard]] std::optional<core::Error> ConsumePostgresAcquireFailureForTesting() {
  std::lock_guard<std::mutex> lock(g_test_acquire_failure_mutex);
  if (!g_test_acquire_failure.has_value()) {
    return std::nullopt;
  }
  auto error = std::move(g_test_acquire_failure);
  g_test_acquire_failure.reset();
  return error;
}
}  // namespace
#endif

class PostgresConnectionPool final {
 public:
  explicit PostgresConnectionPool(std::string connection_string, std::size_t size = kDefaultPostgresConnectionPoolSize)
      : connection_string_(std::move(connection_string)) {
    connections_.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
      auto connection = OpenConnection();
      if (!connection.ok()) {
        throw std::runtime_error(std::string(connection.error().message()));
      }
      connections_.push_back(std::move(connection.value()));
    }
  }

  class Lease final {
   public:
    Lease(PostgresConnectionPool* pool, PgConnection connection) : pool_(pool), connection_(std::move(connection)) {}
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept : pool_(other.pool_), connection_(std::move(other.connection_)) {
      other.pool_ = nullptr;
    }
    Lease& operator=(Lease&& other) noexcept = delete;
    ~Lease() {
      if (pool_ != nullptr && connection_ != nullptr) {
        pool_->Return(std::move(connection_));
      }
    }
    [[nodiscard]] PGconn* get() const noexcept { return connection_.get(); }

   private:
    PostgresConnectionPool* pool_ = nullptr;
    PgConnection connection_;
  };

  [[nodiscard]] core::Result<Lease> Acquire() {
#ifdef A2A_POSTGRES_STORE_TESTING
    if (auto failure = ConsumePostgresAcquireFailureForTesting(); failure.has_value()) {
      return std::move(*failure);
    }
#endif

    PgConnection connection;
    {
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

 private:
  [[nodiscard]] core::Result<PgConnection> OpenConnection() const {
    PgConnection connection(PQconnectdb(connection_string_.c_str()));
    if (connection == nullptr || PQstatus(connection.get()) != CONNECTION_OK) {
      const std::string message =
          connection == nullptr ? "unable to allocate PostgreSQL connection" : PQerrorMessage(connection.get());
      return DatabaseConnectionError(message);
    }
    return std::move(connection);
  }

  void Return(PgConnection connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.push_back(std::move(connection));
    condition_.notify_one();
  }

  std::string connection_string_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<PgConnection> connections_;
};

namespace {

[[nodiscard]] core::Result<void> InitializeSchema(PGconn* connection, const PostgresStoreOptions& options) {
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
  const std::string create_tasks = "CREATE TABLE IF NOT EXISTS " + tasks +
                                   " (id TEXT PRIMARY KEY, context_id TEXT NOT NULL, state INTEGER NOT NULL, "
                                   "status_seconds BIGINT NOT NULL DEFAULT 0, status_nanos INTEGER NOT NULL DEFAULT 0, "
                                   "task_proto BYTEA NOT NULL, updated_at TIMESTAMPTZ NOT NULL DEFAULT now());";
  constexpr std::string_view kTasksUpdatedIndex = "idx_a2a_tasks_updated";
  constexpr std::string_view kTasksContextIndex = "idx_a2a_tasks_context";
  constexpr std::string_view kTasksStateIndex = "idx_a2a_tasks_state";
  constexpr std::string_view kTasksUpdatedIndexColumns = "(status_seconds DESC, status_nanos DESC, id DESC)";
  constexpr std::string_view kTasksContextIndexColumns =
      "(context_id, status_seconds DESC, status_nanos DESC, id DESC)";
  constexpr std::string_view kTasksStateIndexColumns = "(state, status_seconds DESC, status_nanos DESC, id DESC)";
  const std::string create_tasks_updated_index =
      CreateIndexStatement(kTasksUpdatedIndex, tasks, kTasksUpdatedIndexColumns);
  const std::string create_tasks_context_index =
      CreateIndexStatement(kTasksContextIndex, tasks, kTasksContextIndexColumns);
  const std::string create_tasks_state_index = CreateIndexStatement(kTasksStateIndex, tasks, kTasksStateIndexColumns);
  const std::string create_push_configs = "CREATE TABLE IF NOT EXISTS " + push_configs +
                                          " (task_id TEXT NOT NULL, config_id TEXT NOT NULL, url TEXT NOT NULL, "
                                          "config_proto BYTEA NOT NULL, updated_at TIMESTAMPTZ NOT NULL DEFAULT now(), "
                                          "PRIMARY KEY (task_id, config_id));";
  constexpr std::string_view kPushConfigsTaskIndex = "idx_a2a_push_configs_task";
  constexpr std::string_view kPushConfigsTaskIndexColumns = "(task_id)";
  const std::string create_push_configs_task_index =
      CreateIndexStatement(kPushConfigsTaskIndex, push_configs, kPushConfigsTaskIndexColumns);

  const std::vector<std::string> schema_statements = {
      create_tasks,        create_tasks_updated_index,    create_tasks_context_index, create_tasks_state_index,
      create_push_configs, create_push_configs_task_index};
  for (const auto& statement : schema_statements) {
    const auto executed = Exec(connection, statement, "initialize postgres store schema");
    if (!executed.ok()) {
      return executed.error();
    }
  }
  return {};
}

[[nodiscard]] std::shared_ptr<PostgresConnectionPool> MakePool(const PostgresStoreOptions& options) {
  ValidatePostgresStoreOptionsOrThrow(options);
  return std::make_shared<PostgresConnectionPool>(options.connection_string);
}

[[nodiscard]] core::Result<void> UpsertTask(PGconn* connection, const PostgresStoreOptions& options,
                                            const lf::a2a::v1::Task& task) {
  const std::string payload = task.SerializeAsString();
  const std::string state = std::to_string(static_cast<int>(task.status().state()));
  const std::string seconds = std::to_string(task.status().has_timestamp() ? task.status().timestamp().seconds() : 0);
  const std::string nanos = std::to_string(task.status().has_timestamp() ? task.status().timestamp().nanos() : 0);
  const std::string sql = "INSERT INTO " + TaskTable(options.schema) +
                          " (id, context_id, state, status_seconds, status_nanos, task_proto, updated_at) "
                          "VALUES ($1, $2, $3, $4, $5, $6, now()) "
                          "ON CONFLICT (id) DO UPDATE SET context_id = EXCLUDED.context_id, state = EXCLUDED.state, "
                          "status_seconds = EXCLUDED.status_seconds, status_nanos = EXCLUDED.status_nanos, "
                          "task_proto = EXCLUDED.task_proto, updated_at = now()";
  const char* values[] = {task.id().c_str(), task.context_id().c_str(), state.c_str(), seconds.c_str(), nanos.c_str(),
                          payload.data()};
  const int lengths[] = {0, 0, 0, 0, 0, static_cast<int>(payload.size())};
  const int formats[] = {0, 0, 0, 0, 0, 1};
  constexpr int kTaskUpsertParameterCount = 6;
  PgResult result(
      PQexecParams(connection, sql.c_str(), kTaskUpsertParameterCount, nullptr, values, lengths, formats, 0));
  return CheckCommand(connection, result.get(), "upsert postgres task");
}

[[nodiscard]] core::Result<lf::a2a::v1::Task> ParseTaskRow(PGresult* result, int row) {
  lf::a2a::v1::Task task;
  const auto* bytes = PQgetvalue(result, row, 0);
  const int size = PQgetlength(result, row, 0);
  if (!task.ParseFromArray(bytes, size)) {
    return core::Error::Serialization("failed to parse stored Task protobuf");
  }
  return task;
}

[[nodiscard]] core::Result<lf::a2a::v1::Task> SelectTaskForUpdate(PGconn* connection,
                                                                  const PostgresStoreOptions& options,
                                                                  std::string_view id) {
  const std::string id_value(id);
  const std::string sql = "SELECT task_proto FROM " + TaskTable(options.schema) + " WHERE id = $1 FOR UPDATE";
  const char* values[] = {id_value.c_str()};
  PgResult result(PQexecParams(connection, sql.c_str(), 1, nullptr, values, nullptr, nullptr, 1));
  const auto checked = CheckTuples(connection, result.get(), "select postgres task for update");
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) == 0) {
    return core::protocol_errors::TaskNotFound(std::string(kTaskNotFoundMessage));
  }
  return ParseTaskRow(result.get(), 0);
}

[[nodiscard]] PostgresConnectionPool::Lease AcquireOrThrow(PostgresConnectionPool& pool) {
  auto lease = pool.Acquire();
  if (!lease.ok()) {
    throw std::runtime_error(std::string(lease.error().message()));
  }
  return std::move(lease.value());
}

[[nodiscard]] std::string AddSqlParameter(std::vector<std::string>* values, std::string value) {
  values->push_back(std::move(value));
  return "$" + std::to_string(values->size());
}

[[nodiscard]] std::vector<const char*> BuildSqlParameterPointers(const std::vector<std::string>& values) {
  std::vector<const char*> pointers;
  pointers.reserve(values.size());
  for (const auto& value : values) {
    pointers.push_back(value.c_str());
  }
  return pointers;
}

struct TaskListSqlFilter final {
  std::string where_clause;
  std::vector<std::string> values;
};

[[nodiscard]] TaskListSqlFilter BuildTaskListSqlFilter(const ListTasksRequest& request) {
  TaskListSqlFilter filter;
  std::vector<std::string> predicates;

  if (!request.context_id.empty()) {
    predicates.push_back("context_id = " + AddSqlParameter(&filter.values, request.context_id));
  }
  if (request.status_filter.has_value()) {
    predicates.push_back("state = " +
                         AddSqlParameter(&filter.values, std::to_string(static_cast<int>(*request.status_filter))));
  }
  if (request.status_timestamp_after.has_value()) {
    const auto& cutoff = *request.status_timestamp_after;
    const std::string seconds_param = AddSqlParameter(&filter.values, std::to_string(cutoff.seconds()));
    const std::string nanos_param = AddSqlParameter(&filter.values, std::to_string(cutoff.nanos()));
    predicates.push_back("(status_seconds > " + seconds_param + " OR (status_seconds = " + seconds_param +
                         " AND status_nanos >= " + nanos_param + "))");
  }

  if (!predicates.empty()) {
    filter.where_clause = " WHERE ";
    for (std::size_t index = 0; index < predicates.size(); ++index) {
      if (index != 0) {
        filter.where_clause += " AND ";
      }
      filter.where_clause += predicates[index];
    }
  }
  return filter;
}

[[nodiscard]] core::Result<std::size_t> ParseCountResult(PGresult* result, std::string_view operation) {
  if (PQntuples(result) != 1) {
    return core::Error::Internal(std::string(operation) + ": expected exactly one count row");
  }
  std::size_t parsed = 0;
  const std::string_view raw_count(PQgetvalue(result, 0, 0));
  const auto parsed_result = std::from_chars(raw_count.data(), raw_count.data() + raw_count.size(), parsed);
  if (parsed_result.ec != std::errc() || parsed_result.ptr != raw_count.data() + raw_count.size()) {
    return core::Error::Internal(std::string(operation) + ": failed to parse count");
  }
  return parsed;
}

}  // namespace

PostgresTaskStore::PostgresTaskStore(PostgresStoreOptions options)
    : pool_(MakePool(options)), options_(std::move(options)) {
  auto lease = AcquireOrThrow(*pool_);
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
}

PostgresTaskStore::PostgresTaskStore(std::shared_ptr<PostgresConnectionPool> pool, PostgresStoreOptions options)
    : pool_(std::move(pool)), options_(std::move(options)) {
  ValidatePostgresStoreOptionsOrThrow(options_);
  auto lease = AcquireOrThrow(*pool_);
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
}

PostgresTaskStore::~PostgresTaskStore() = default;

core::Result<void> PostgresTaskStore::CreateOrUpdate(const lf::a2a::v1::Task& task) {
  if (task.id().empty()) {
    return core::Error::Validation(std::string(kTaskIdFieldRequiredMessage));
  }
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  return UpsertTask(lease.value().get(), options_, task);
}

core::Result<lf::a2a::v1::Task> PostgresTaskStore::Get(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  const std::string id_value(id);
  const std::string sql = "SELECT task_proto FROM " + TaskTable(options_.schema) + " WHERE id = $1";
  const char* values[] = {id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result(PQexecParams(lease.value().get(), sql.c_str(), 1, nullptr, values, nullptr, nullptr, 1));
  const auto checked = CheckTuples(lease.value().get(), result.get(), "get postgres task");
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) == 0) {
    return core::protocol_errors::TaskNotFound(std::string(kTaskNotFoundMessage));
  }
  return ParseTaskRow(result.get(), 0);
}

core::Result<ListTasksResponse> PostgresTaskStore::List(const ListTasksRequest& request) const {
  const auto offset = ParseListPageToken(request.page_token);
  if (!offset.ok()) {
    return offset.error();
  }

  const TaskListSqlFilter filter = BuildTaskListSqlFilter(request);
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }

  const std::string count_sql = "SELECT count(*) FROM " + TaskTable(options_.schema) + filter.where_clause;
  const auto count_values = BuildSqlParameterPointers(filter.values);
  PgResult count_result(PQexecParams(lease.value().get(), count_sql.c_str(), static_cast<int>(count_values.size()),
                                     nullptr, count_values.data(), nullptr, nullptr, 0));
  const auto count_checked = CheckTuples(lease.value().get(), count_result.get(), "count postgres tasks");
  if (!count_checked.ok()) {
    return count_checked.error();
  }
  const auto total_size = ParseCountResult(count_result.get(), "count postgres tasks");
  if (!total_size.ok()) {
    return total_size.error();
  }

  const auto valid_offset = ValidateListPageOffset(offset.value(), total_size.value());
  if (!valid_offset.ok()) {
    return valid_offset.error();
  }

  ListTasksResponse response;
  response.total_size = total_size.value();
  const std::size_t remaining = total_size.value() - offset.value();
  const std::size_t effective_page_size = request.page_size == 0 ? remaining : request.page_size;
  const std::size_t result_size = std::min(effective_page_size, remaining);

  std::vector<std::string> select_values = filter.values;
  std::string select_sql = "SELECT task_proto FROM " + TaskTable(options_.schema) + filter.where_clause +
                           " ORDER BY status_seconds DESC, status_nanos DESC, id DESC";
  if (request.page_size != 0) {
    select_sql += " LIMIT " + AddSqlParameter(&select_values, std::to_string(result_size));
  }
  select_sql += " OFFSET " + AddSqlParameter(&select_values, std::to_string(offset.value()));

  const auto select_value_pointers = BuildSqlParameterPointers(select_values);
  PgResult result(PQexecParams(lease.value().get(), select_sql.c_str(), static_cast<int>(select_value_pointers.size()),
                               nullptr, select_value_pointers.data(), nullptr, nullptr, 1));
  const auto checked = CheckTuples(lease.value().get(), result.get(), "list postgres tasks");
  if (!checked.ok()) {
    return checked.error();
  }

  response.tasks.reserve(result_size);
  for (int row = 0; row < PQntuples(result.get()); ++row) {
    auto task = ParseTaskRow(result.get(), row);
    if (!task.ok()) {
      return task.error();
    }
    ApplyArtifactProjection(&task.value(), request.include_artifacts);
    ApplyHistoryRetention(&task.value(), request.history_length);
    response.tasks.push_back(std::move(task.value()));
  }
  response.page_size = response.tasks.size();
  if (request.page_size != 0 && result_size < remaining) {
    response.next_page_token = std::to_string(offset.value() + result_size);
  }
  return response;
}

core::Result<lf::a2a::v1::Task> PostgresTaskStore::Cancel(std::string_view id) {
  if (id.empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  Transaction transaction(lease.value().get());
  auto begun = transaction.Begin();
  if (!begun.ok()) {
    return begun.error();
  }
  auto task = SelectTaskForUpdate(lease.value().get(), options_, id);
  if (!task.ok()) {
    return task.error();
  }
  if (core::IsTerminalTaskState(task.value().status().state())) {
    return core::protocol_errors::TaskNotCancelable();
  }
  task.value().mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
  auto updated = UpsertTask(lease.value().get(), options_, task.value());
  if (!updated.ok()) {
    return updated.error();
  }
  auto committed = transaction.Commit();
  if (!committed.ok()) {
    return committed.error();
  }
  return task.value();
}

core::Result<lf::a2a::v1::Task> PostgresTaskStore::AppendTaskHistory(std::string_view task_id,
                                                                     const lf::a2a::v1::Message& message,
                                                                     HistoryAppendPolicy policy) {
  if (task_id.empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  Transaction transaction(lease.value().get());
  auto begun = transaction.Begin();
  if (!begun.ok()) {
    return begun.error();
  }
  auto task = SelectTaskForUpdate(lease.value().get(), options_, task_id);
  if (!task.ok()) {
    return task.error();
  }
  const auto dedupe_reason = FindHistoryDedupeReason(task.value().history(), message, policy);
  if (dedupe_reason.has_value()) {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    UpdateDedupeSnapshot(&telemetry_snapshot_, *dedupe_reason);
  } else {
    *task.value().add_history() = message;
    auto updated = UpsertTask(lease.value().get(), options_, task.value());
    if (!updated.ok()) {
      return updated.error();
    }
  }
  auto committed = transaction.Commit();
  if (!committed.ok()) {
    return committed.error();
  }
  return task.value();
}

TaskStore::HistoryTelemetrySnapshot PostgresTaskStore::GetHistoryTelemetrySnapshot() const {
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  return telemetry_snapshot_;
}

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

#ifdef A2A_POSTGRES_STORE_TESTING
void FailNextPostgresAcquireForTesting(core::Error error) {
  std::lock_guard<std::mutex> lock(g_test_acquire_failure_mutex);
  g_test_acquire_failure = std::move(error);
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
