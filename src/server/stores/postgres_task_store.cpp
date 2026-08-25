// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/postgres_task_store.h"

#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"
#include "a2a/server/tasks/task_history.h"

namespace a2a::server::stores {
namespace {

constexpr std::size_t kTaskUpsertSqlReserve = 520U;
constexpr std::size_t kTaskSnapshotSqlReserve = 15U;
constexpr std::size_t kTaskHistorySnapshotSqlReserve = 15U;
constexpr std::size_t kConditionalTaskWriteSqlReserve = 300U;
constexpr std::size_t kTaskHistoryUpdateSqlReserve = 160U;
constexpr std::size_t kTaskHistoryRevisionRetryLimit = 8U;
constexpr int kTaskHistoryRevisionColumn = 1;
constexpr int kTaskHistoryCreatedSequenceColumn = 2;
constexpr std::string_view kNoRowsAffected = "0";
constexpr std::string_view kTaskHistoryRevisionRetryLimitExceededMessage =
    "task history append exceeded revision conflict retry limit";
constexpr std::string_view kTaskHistoryRevisionParseErrorMessage = "failed to parse postgres task history revision";
constexpr std::string_view kTaskHistoryCreatedSequenceParseErrorMessage =
    "failed to parse postgres task history created sequence";

[[nodiscard]] std::string BuildTaskSnapshotSql(std::string_view schema) {
  std::string sql = "SELECT task_proto, revision::text FROM ";
  const std::string table = TaskTable(schema);
  sql.reserve(sql.size() + table.size() + kTaskSnapshotSqlReserve);
  sql.append(table);
  sql.append(" WHERE id = $1");
  return sql;
}

[[nodiscard]] std::string BuildTaskHistorySnapshotSql(std::string_view schema) {
  std::string sql = "SELECT task_proto, revision::text, created_sequence::text FROM ";
  const std::string table = TaskTable(schema);
  sql.reserve(sql.size() + table.size() + kTaskHistorySnapshotSqlReserve);
  sql.append(table);
  sql.append(" WHERE id = $1");
  return sql;
}

[[nodiscard]] std::string BuildConditionalTaskCreateSql(std::string_view schema) {
  std::string sql = "INSERT INTO ";
  sql.reserve(sql.size() + schema.size() + kConditionalTaskWriteSqlReserve);
  sql.append(TaskTable(schema));
  sql.append(
      " (id, context_id, state, has_status_timestamp, status_seconds, status_nanos, task_proto) "
      "VALUES ($1, $2, $3, $4, $5, $6, $7) ON CONFLICT (id) DO NOTHING RETURNING revision");
  return sql;
}

[[nodiscard]] std::string BuildConditionalTaskUpdateSql(std::string_view schema) {
  std::string sql = "UPDATE ";
  sql.reserve(sql.size() + schema.size() + kConditionalTaskWriteSqlReserve);
  sql.append(TaskTable(schema));
  sql.append(
      " SET context_id = $2, state = $3, has_status_timestamp = $4, status_seconds = $5, "
      "status_nanos = $6, task_proto = $7, revision = revision + 1, updated_at = now() "
      "WHERE id = $1 AND revision = $8 RETURNING revision");
  return sql;
}

[[nodiscard]] core::Result<void> UpsertTask(PGconn* connection, const PostgresStoreOptions& options,
                                            const lf::a2a::v1::Task& task) {
  const std::string table = TaskTable(options.schema);
  const std::string payload = task.SerializeAsString();
  const bool has_status_timestamp = task.status().has_timestamp();
  const std::string has_timestamp = has_status_timestamp ? "true" : "false";
  const std::string state = std::to_string(static_cast<int>(task.status().state()));
  const std::string seconds = std::to_string(has_status_timestamp ? task.status().timestamp().seconds() : 0);
  const std::string nanos = std::to_string(has_status_timestamp ? task.status().timestamp().nanos() : 0);
  std::string sql = "INSERT INTO ";
  sql.reserve(sql.size() + table.size() + kTaskUpsertSqlReserve);
  sql.append(table);
  sql.append(
      " AS target"
      " (id, context_id, state, has_status_timestamp, status_seconds, status_nanos, task_proto, updated_at) "
      "VALUES ($1, $2, $3, $4, $5, $6, $7, now()) "
      "ON CONFLICT (id) DO UPDATE SET context_id = EXCLUDED.context_id, state = EXCLUDED.state, "
      "has_status_timestamp = EXCLUDED.has_status_timestamp, status_seconds = EXCLUDED.status_seconds, "
      "status_nanos = EXCLUDED.status_nanos, task_proto = EXCLUDED.task_proto, "
      "revision = target.revision + 1, updated_at = now()");
  constexpr int kTaskUpsertParameterCount = 7;
  const std::array<const char*, kTaskUpsertParameterCount> values = {task.id().c_str(), task.context_id().c_str(),
                                                                     state.c_str(),     has_timestamp.c_str(),
                                                                     seconds.c_str(),   nanos.c_str(),
                                                                     payload.data()};
  const std::array<int, kTaskUpsertParameterCount> lengths = {0, 0, 0, 0, 0, 0, static_cast<int>(payload.size())};
  const std::array<int, kTaskUpsertParameterCount> formats = {0, 0, 0, 0, 0, 0, 1};
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTaskUpsert);
#endif
    result.reset(PQexecParams(connection, sql.c_str(), kTaskUpsertParameterCount, nullptr, values.data(),
                              lengths.data(), formats.data(), 0));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  return CheckCommand(connection, result.get(), "upsert postgres task");
}

[[nodiscard]] core::Result<bool> UpdateTaskHistory(PGconn* connection, const PostgresStoreOptions& options,
                                                   const lf::a2a::v1::Task& task, std::uint64_t expected_revision,
                                                   std::uint64_t expected_created_sequence) {
  const std::string payload = task.SerializeAsString();
  const std::string revision = std::to_string(expected_revision);
  const std::string created_sequence = std::to_string(expected_created_sequence);
  const std::string table = TaskTable(options.schema);
  std::string sql;
  sql.reserve(table.size() + kTaskHistoryUpdateSqlReserve);
  sql.append("UPDATE ");
  sql.append(table);
  sql.append(
      " SET task_proto = $2, revision = revision + 1, updated_at = now() "
      "WHERE id = $1 AND revision = $3 AND created_sequence = $4");
  constexpr int kTaskHistoryUpdateParameterCount = 4;
  const std::array<const char*, kTaskHistoryUpdateParameterCount> values = {task.id().c_str(), payload.data(),
                                                                            revision.c_str(), created_sequence.c_str()};
  const std::array<int, kTaskHistoryUpdateParameterCount> lengths = {0, static_cast<int>(payload.size()), 0, 0};
  const std::array<int, kTaskHistoryUpdateParameterCount> formats = {0, 1, 0, 0};
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTaskUpsert);
#endif
    result.reset(PQexecParams(connection, sql.c_str(), kTaskHistoryUpdateParameterCount, nullptr, values.data(),
                              lengths.data(), formats.data(), 0));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  const auto checked = CheckCommand(connection, result.get(), "update postgres task history");
  if (!checked.ok()) {
    return checked.error();
  }
  return std::string_view(PQcmdTuples(result.get())) != kNoRowsAffected;
}

struct TaskHistorySnapshot final {
  lf::a2a::v1::Task task;
  std::uint64_t revision = 0;
  std::uint64_t created_sequence = 0;
};

[[nodiscard]] core::Result<lf::a2a::v1::Task> ParseTaskRow(PGresult* result, int row);

[[nodiscard]] core::Result<std::uint64_t> ParseTaskHistoryUnsignedColumn(PGresult* result, int row, int column,
                                                                         std::string_view error_message) {
  const std::string_view raw_value(PQgetvalue(result, row, column));
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(raw_value.data(), raw_value.data() + raw_value.size(), value);
  if (parsed.ec != std::errc() || parsed.ptr != raw_value.data() + raw_value.size()) {
    return core::Error::Serialization(std::string(error_message));
  }
  return value;
}

[[nodiscard]] core::Result<TaskHistorySnapshot> SelectTaskHistorySnapshot(PGconn* connection, const std::string& sql,
                                                                          std::string_view id) {
  const std::string id_value(id);
  const std::array<const char*, 1> values = {id_value.c_str()};
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTaskHistorySnapshot);
#endif
    result.reset(PQexecParams(connection, sql.c_str(), 1, nullptr, values.data(), nullptr, nullptr, 1));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  const auto checked = CheckTuples(connection, result.get(), "select postgres task history snapshot");
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) == 0) {
    return core::protocol_errors::TaskNotFound(std::string(kTaskNotFoundMessage));
  }
  auto task = ParseTaskRow(result.get(), 0);
  if (!task.ok()) {
    return task.error();
  }
  const auto revision = ParseTaskHistoryUnsignedColumn(result.get(), 0, kTaskHistoryRevisionColumn,
                                                       kTaskHistoryRevisionParseErrorMessage);
  if (!revision.ok()) {
    return revision.error();
  }
  const auto created_sequence = ParseTaskHistoryUnsignedColumn(result.get(), 0, kTaskHistoryCreatedSequenceColumn,
                                                               kTaskHistoryCreatedSequenceParseErrorMessage);
  if (!created_sequence.ok()) {
    return created_sequence.error();
  }
  return TaskHistorySnapshot{
      .task = std::move(task.value()), .revision = revision.value(), .created_sequence = created_sequence.value()};
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
                                                                  std::string_view id, bool record_history_diagnostic) {
  const std::string id_value(id);
  const std::string sql = "SELECT task_proto FROM " + TaskTable(options.schema) + " WHERE id = $1 FOR UPDATE";
  const std::array<const char*, 1> values = {id_value.c_str()};
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  if (record_history_diagnostic) {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTaskHistoryLockRead);
    result.reset(PQexecParams(connection, sql.c_str(), 1, nullptr, values.data(), nullptr, nullptr, 1));
  } else {
    result.reset(PQexecParams(connection, sql.c_str(), 1, nullptr, values.data(), nullptr, nullptr, 1));
  }
#else
  (void)record_history_diagnostic;
  result.reset(PQexecParams(connection, sql.c_str(), 1, nullptr, values.data(), nullptr, nullptr, 1));
#endif
  const auto checked = CheckTuples(connection, result.get(), "select postgres task for update");
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) == 0) {
    return core::protocol_errors::TaskNotFound(std::string(kTaskNotFoundMessage));
  }
  return ParseTaskRow(result.get(), 0);
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
    predicates.push_back("(has_status_timestamp = TRUE AND (status_seconds > " + seconds_param +
                         " OR (status_seconds = " + seconds_param + " AND status_nanos >= " + nanos_param + ")))");
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
    : pool_(MakePool(options)),
      options_(std::move(options)),
      storage_identity_(pool_->StorageCoordinates(options_.schema, options_.storage_authority_id)),
      execution_identity_(pool_->ExecutionIdentity(options_.schema, options_.storage_authority_id)),
      snapshot_sql_(BuildTaskSnapshotSql(options_.schema)),
      history_snapshot_sql_(BuildTaskHistorySnapshotSql(options_.schema)),
      conditional_create_sql_(BuildConditionalTaskCreateSql(options_.schema)),
      conditional_update_sql_(BuildConditionalTaskUpdateSql(options_.schema)) {
  auto lease = AcquireOrThrow(*pool_);
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
}

PostgresTaskStore::PostgresTaskStore(std::shared_ptr<PostgresConnectionPool> pool, PostgresStoreOptions options)
    : pool_(std::move(pool)),
      options_(std::move(options)),
      storage_identity_(pool_->StorageCoordinates(options_.schema, options_.storage_authority_id)),
      execution_identity_(pool_->ExecutionIdentity(options_.schema, options_.storage_authority_id)),
      snapshot_sql_(BuildTaskSnapshotSql(options_.schema)),
      history_snapshot_sql_(BuildTaskHistorySnapshotSql(options_.schema)),
      conditional_create_sql_(BuildConditionalTaskCreateSql(options_.schema)),
      conditional_update_sql_(BuildConditionalTaskUpdateSql(options_.schema)) {
  ValidatePostgresStoreOptionsOrThrow(options_);
  auto lease = AcquireOrThrow(*pool_);
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
}

PostgresTaskStore::~PostgresTaskStore() = default;

bool PostgresTaskStore::UsesStorage(const PostgresStorageIdentity& identity) const noexcept {
  return ClassifyPostgresStorageAuthority(storage_identity_, identity) == PostgresStorageAuthority::kLocal;
}

bool PostgresTaskStore::SharesConnectionPool(const PostgresConnectionPool& pool) const noexcept {
  return pool_.get() == &pool;
}

const PostgresStorageIdentity& PostgresTaskStore::storage_identity() const noexcept { return storage_identity_; }

const PostgresExecutionIdentity& PostgresTaskStore::execution_identity() const noexcept { return execution_identity_; }

#ifdef A2A_POSTGRES_STORE_TESTING
void PostgresTaskStore::SetHistorySnapshotHookForTesting(std::function<void()> hook) {
  history_snapshot_hook_for_testing_ = std::move(hook);
}

const PostgresConnectionPool* PostgresTaskStore::connection_pool_for_testing() const noexcept { return pool_.get(); }
#endif

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

core::Result<TaskStore::TaskSnapshot> PostgresTaskStore::GetSnapshot(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  const std::string id_value(id);
  const std::array<const char*, 1> values = {id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTaskGet);
#endif
    result.reset(
        PQexecParams(lease.value().get(), snapshot_sql_.c_str(), 1, nullptr, values.data(), nullptr, nullptr, 1));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  const auto checked = CheckTuples(lease.value().get(), result.get(), "get postgres task snapshot");
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) == 0) {
    return core::protocol_errors::TaskNotFound(std::string(kTaskNotFoundMessage));
  }
  auto task = ParseTaskRow(result.get(), 0);
  if (!task.ok()) {
    return task.error();
  }
  std::uint64_t revision = 0;
  const std::string_view raw_revision(PQgetvalue(result.get(), 0, 1));
  const auto parsed = std::from_chars(raw_revision.data(), raw_revision.data() + raw_revision.size(), revision);
  if (parsed.ec != std::errc() || parsed.ptr != raw_revision.data() + raw_revision.size()) {
    return core::Error::Serialization("failed to parse postgres task revision");
  }
  return TaskSnapshot{.task = std::move(task.value()), .revision = revision};
}

core::Result<TaskStore::ConditionalWriteResult> PostgresTaskStore::CreateOrUpdateIfRevision(
    const lf::a2a::v1::Task& task, std::uint64_t expected_revision) {
  if (task.id().empty()) {
    return core::Error::Validation(std::string(kTaskIdFieldRequiredMessage));
  }
  const std::string payload = task.SerializeAsString();
  const bool has_status_timestamp = task.status().has_timestamp();
  const std::string has_timestamp = has_status_timestamp ? "true" : "false";
  const std::string state = std::to_string(static_cast<int>(task.status().state()));
  const std::string seconds = std::to_string(has_status_timestamp ? task.status().timestamp().seconds() : 0);
  const std::string nanos = std::to_string(has_status_timestamp ? task.status().timestamp().nanos() : 0);
  const std::string revision = std::to_string(expected_revision);
  const std::string& sql = expected_revision == 0 ? conditional_create_sql_ : conditional_update_sql_;
  constexpr int kCreateParameterCount = 7;
  constexpr int kUpdateParameterCount = 8;
  const std::array<const char*, kUpdateParameterCount> values = {task.id().c_str(), task.context_id().c_str(),
                                                                 state.c_str(),     has_timestamp.c_str(),
                                                                 seconds.c_str(),   nanos.c_str(),
                                                                 payload.data(),    revision.c_str()};
  const std::array<int, kUpdateParameterCount> lengths = {0, 0, 0, 0, 0, 0, static_cast<int>(payload.size()), 0};
  const std::array<int, kUpdateParameterCount> formats = {0, 0, 0, 0, 0, 0, 1, 0};
  const int parameter_count = expected_revision == 0 ? kCreateParameterCount : kUpdateParameterCount;
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTaskUpsert);
#endif
    result.reset(PQexecParams(lease.value().get(), sql.c_str(), parameter_count, nullptr, values.data(), lengths.data(),
                              formats.data(), 0));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
  const auto checked = CheckTuples(lease.value().get(), result.get(), "conditionally persist postgres task");
  if (!checked.ok()) {
    return checked.error();
  }
  return PQntuples(result.get()) == 0 ? ConditionalWriteResult::kConflict : ConditionalWriteResult::kUpdated;
}

core::Result<lf::a2a::v1::Task> PostgresTaskStore::Get(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  const std::string id_value(id);
  const std::string sql = "SELECT task_proto FROM " + TaskTable(options_.schema) + " WHERE id = $1";
  const std::array<const char*, 1> values = {id_value.c_str()};
  auto lease = pool_->Acquire();
  if (!lease.ok()) {
    return lease.error();
  }
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTaskGet);
#endif
    result.reset(PQexecParams(lease.value().get(), sql.c_str(), 1, nullptr, values.data(), nullptr, nullptr, 1));
#ifdef A2A_POSTGRES_STORE_TESTING
  }
#endif
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
  std::string select_sql =
      "SELECT task_proto FROM " + TaskTable(options_.schema) + filter.where_clause + " ORDER BY created_sequence ASC";
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
  auto task = SelectTaskForUpdate(lease.value().get(), options_, id, false);
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
  for (std::size_t attempt = 0; attempt < kTaskHistoryRevisionRetryLimit; ++attempt) {
    auto snapshot = SelectTaskHistorySnapshot(lease.value().get(), history_snapshot_sql_, task_id);
    if (!snapshot.ok()) {
      return snapshot.error();
    }
    const auto dedupe_reason = FindHistoryDedupeReason(snapshot.value().task.history(), message, policy);
    if (dedupe_reason.has_value()) {
      std::lock_guard<std::mutex> lock(telemetry_mutex_);
      UpdateDedupeSnapshot(&telemetry_snapshot_, *dedupe_reason);
      return snapshot.value().task;
    }
#ifdef A2A_POSTGRES_STORE_TESTING
    if (history_snapshot_hook_for_testing_) {
      history_snapshot_hook_for_testing_();
    }
#endif
    *snapshot.value().task.add_history() = message;
    auto updated = UpdateTaskHistory(lease.value().get(), options_, snapshot.value().task, snapshot.value().revision,
                                     snapshot.value().created_sequence);
    if (!updated.ok()) {
      return updated.error();
    }
    if (updated.value()) {
      return snapshot.value().task;
    }
  }
  return core::Error::Internal(std::string(kTaskHistoryRevisionRetryLimitExceededMessage));
}

TaskStore::HistoryTelemetrySnapshot PostgresTaskStore::GetHistoryTelemetrySnapshot() const {
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  return telemetry_snapshot_;
}

}  // namespace a2a::server::stores
