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
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"

namespace a2a::server::stores {
namespace {

constexpr std::size_t kDefaultPoolSize = 4;
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

[[nodiscard]] bool IsValidSchemaName(std::string_view schema) {
  if (schema.empty()) {
    return false;
  }
  const auto is_alpha_or_underscore = [](unsigned char ch) { return std::isalpha(ch) != 0 || ch == '_'; };
  const auto is_alnum_or_underscore = [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; };
  if (!is_alpha_or_underscore(static_cast<unsigned char>(schema.front()))) {
    return false;
  }
  return std::ranges::all_of(schema.substr(1),
                             [&](char ch) { return is_alnum_or_underscore(static_cast<unsigned char>(ch)); });
}

[[nodiscard]] std::string QuoteSqlIdentifier(std::string_view identifier) {
  std::string quoted;
  quoted.reserve(identifier.size() + 2);
  quoted.push_back('"');
  for (const char ch : identifier) {
    if (ch == '"') {
      quoted.push_back('"');
    }
    quoted.push_back(ch);
  }
  quoted.push_back('"');
  return quoted;
}

[[nodiscard]] std::string Qualified(std::string_view schema, std::string_view identifier) {
  return QuoteSqlIdentifier(schema) + "." + QuoteSqlIdentifier(identifier);
}

[[nodiscard]] std::string TaskTable(std::string_view schema) { return Qualified(schema, "a2a_tasks"); }
[[nodiscard]] std::string PushTable(std::string_view schema) {
  return Qualified(schema, "a2a_push_notification_configs");
}
[[nodiscard]] std::string IndexName(std::string_view schema, std::string_view index_name) {
  return Qualified(schema, index_name);
}

[[nodiscard]] core::Result<void> ValidatePostgresStoreOptions(const PostgresStoreOptions& options) {
  if (!IsValidSchemaName(options.schema)) {
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

[[nodiscard]] bool HasStatusAfterCutoff(const lf::a2a::v1::Task& task, const google::protobuf::Timestamp& cutoff) {
  if (!task.status().has_timestamp()) {
    return false;
  }
  const auto& timestamp = task.status().timestamp();
  return timestamp.seconds() > cutoff.seconds() ||
         (timestamp.seconds() == cutoff.seconds() && timestamp.nanos() >= cutoff.nanos());
}

[[nodiscard]] bool MatchesListFilters(const lf::a2a::v1::Task& task, const ListTasksRequest& request) {
  if (!request.context_id.empty() && task.context_id() != request.context_id) {
    return false;
  }
  if (request.status_filter.has_value() && task.status().state() != *request.status_filter) {
    return false;
  }
  if (request.status_timestamp_after.has_value() && !HasStatusAfterCutoff(task, *request.status_timestamp_after)) {
    return false;
  }
  return true;
}

[[nodiscard]] bool HasSameMessageFingerprint(const lf::a2a::v1::Message& lhs, const lf::a2a::v1::Message& rhs) {
  lf::a2a::v1::Message lhs_copy = lhs;
  lf::a2a::v1::Message rhs_copy = rhs;
  lhs_copy.clear_message_id();
  rhs_copy.clear_message_id();
  return lhs_copy.SerializeAsString() == rhs_copy.SerializeAsString();
}

[[nodiscard]] bool HasSameMessageIdAndFingerprint(const lf::a2a::v1::Message& lhs, const lf::a2a::v1::Message& rhs) {
  return !rhs.message_id().empty() && lhs.message_id() == rhs.message_id() && HasSameMessageFingerprint(lhs, rhs);
}

[[nodiscard]] std::optional<TaskStore::HistoryDedupeEvent::Reason> FindHistoryDedupeReason(
    const google::protobuf::RepeatedPtrField<lf::a2a::v1::Message>& history, const lf::a2a::v1::Message& message,
    TaskStore::HistoryAppendPolicy policy) {
  const bool has_message_id = !message.message_id().empty();
  for (const auto& existing : history) {
    if (policy == TaskStore::HistoryAppendPolicy::kDedupByMessageId && has_message_id &&
        HasSameMessageIdAndFingerprint(existing, message)) {
      return TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint;
    }
    if (policy == TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint) {
      if (has_message_id && HasSameMessageIdAndFingerprint(existing, message)) {
        return TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint;
      }
      if (!has_message_id && HasSameMessageFingerprint(existing, message)) {
        return TaskStore::HistoryDedupeEvent::Reason::kDuplicateFingerprintWithoutMessageId;
      }
    }
  }
  return std::nullopt;
}

void UpdateDedupeSnapshot(TaskStore::HistoryTelemetrySnapshot* snapshot, TaskStore::HistoryDedupeEvent::Reason reason) {
  snapshot->dedupe_dropped_total += 1;
  if (reason == TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint) {
    snapshot->dedupe_dropped_by_message_id_and_fingerprint += 1;
  } else {
    snapshot->dedupe_dropped_by_fingerprint_without_message_id += 1;
  }
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

class PostgresConnectionPool final {
 public:
  explicit PostgresConnectionPool(std::string connection_string, std::size_t size = kDefaultPoolSize)
      : connection_string_(std::move(connection_string)) {
    connections_.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
      connections_.push_back(OpenConnection());
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

  [[nodiscard]] Lease Acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_.wait(lock, [&] { return !connections_.empty(); });
    PgConnection connection = std::move(connections_.back());
    connections_.pop_back();
    if (PQstatus(connection.get()) != CONNECTION_OK) {
      connection = OpenConnection();
    }
    return {this, std::move(connection)};
  }

 private:
  [[nodiscard]] PgConnection OpenConnection() const {
    PgConnection connection(PQconnectdb(connection_string_.c_str()));
    if (connection == nullptr || PQstatus(connection.get()) != CONNECTION_OK) {
      const std::string message =
          connection == nullptr ? "unable to allocate PostgreSQL connection" : PQerrorMessage(connection.get());
      throw std::runtime_error(message);
    }
    return connection;
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
  const std::string create_tasks_updated_index = "CREATE INDEX IF NOT EXISTS " +
                                                 IndexName(options.schema, "idx_a2a_tasks_updated") + " ON " + tasks +
                                                 " (status_seconds DESC, status_nanos DESC, id DESC);";
  const std::string create_tasks_context_index = "CREATE INDEX IF NOT EXISTS " +
                                                 IndexName(options.schema, "idx_a2a_tasks_context") + " ON " + tasks +
                                                 " (context_id, status_seconds DESC, status_nanos DESC, id DESC);";
  const std::string create_tasks_state_index = "CREATE INDEX IF NOT EXISTS " +
                                               IndexName(options.schema, "idx_a2a_tasks_state") + " ON " + tasks +
                                               " (state, status_seconds DESC, status_nanos DESC, id DESC);";
  const std::string create_push_configs = "CREATE TABLE IF NOT EXISTS " + push_configs +
                                          " (task_id TEXT NOT NULL, config_id TEXT NOT NULL, url TEXT NOT NULL, "
                                          "config_proto BYTEA NOT NULL, updated_at TIMESTAMPTZ NOT NULL DEFAULT now(), "
                                          "PRIMARY KEY (task_id, config_id));";
  const std::string create_push_configs_task_index = "CREATE INDEX IF NOT EXISTS " +
                                                     IndexName(options.schema, "idx_a2a_push_configs_task") + " ON " +
                                                     push_configs + " (task_id);";

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

}  // namespace

PostgresTaskStore::PostgresTaskStore(PostgresStoreOptions options)
    : pool_(MakePool(options)), options_(std::move(options)) {
  auto lease = pool_->Acquire();
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
}

PostgresTaskStore::PostgresTaskStore(std::shared_ptr<PostgresConnectionPool> pool, PostgresStoreOptions options)
    : pool_(std::move(pool)), options_(std::move(options)) {
  ValidatePostgresStoreOptionsOrThrow(options_);
  auto lease = pool_->Acquire();
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
  return UpsertTask(lease.get(), options_, task);
}

core::Result<lf::a2a::v1::Task> PostgresTaskStore::Get(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation(std::string(kTaskIdRequiredMessage));
  }
  const std::string id_value(id);
  const std::string sql = "SELECT task_proto FROM " + TaskTable(options_.schema) + " WHERE id = $1";
  const char* values[] = {id_value.c_str()};
  auto lease = pool_->Acquire();
  PgResult result(PQexecParams(lease.get(), sql.c_str(), 1, nullptr, values, nullptr, nullptr, 1));
  const auto checked = CheckTuples(lease.get(), result.get(), "get postgres task");
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
  const std::string sql = "SELECT task_proto FROM " + TaskTable(options_.schema) +
                          " ORDER BY status_seconds DESC, status_nanos DESC, id DESC";
  auto lease = pool_->Acquire();
  PgResult result(PQexecParams(lease.get(), sql.c_str(), 0, nullptr, nullptr, nullptr, nullptr, 1));
  const auto checked = CheckTuples(lease.get(), result.get(), "list postgres tasks");
  if (!checked.ok()) {
    return checked.error();
  }

  std::vector<lf::a2a::v1::Task> matched;
  matched.reserve(static_cast<std::size_t>(PQntuples(result.get())));
  for (int row = 0; row < PQntuples(result.get()); ++row) {
    auto task = ParseTaskRow(result.get(), row);
    if (!task.ok()) {
      return task.error();
    }
    if (MatchesListFilters(task.value(), request)) {
      matched.push_back(std::move(task.value()));
    }
  }

  const auto valid_offset = ValidateListPageOffset(offset.value(), matched.size());
  if (!valid_offset.ok()) {
    return valid_offset.error();
  }

  ListTasksResponse response;
  response.total_size = matched.size();
  const std::size_t remaining = matched.size() - offset.value();
  const std::size_t effective_page_size = request.page_size == 0 ? remaining : request.page_size;
  const std::size_t result_size = std::min(effective_page_size, remaining);
  response.tasks.reserve(result_size);
  const std::size_t end = offset.value() + result_size;
  for (std::size_t index = offset.value(); index < end; ++index) {
    ApplyArtifactProjection(&matched[index], request.include_artifacts);
    ApplyHistoryRetention(&matched[index], request.history_length);
    response.tasks.push_back(std::move(matched[index]));
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
  Transaction transaction(lease.get());
  auto begun = transaction.Begin();
  if (!begun.ok()) {
    return begun.error();
  }
  auto task = SelectTaskForUpdate(lease.get(), options_, id);
  if (!task.ok()) {
    return task.error();
  }
  if (core::IsTerminalTaskState(task.value().status().state())) {
    return core::protocol_errors::TaskNotCancelable();
  }
  task.value().mutable_status()->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
  auto updated = UpsertTask(lease.get(), options_, task.value());
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
  Transaction transaction(lease.get());
  auto begun = transaction.Begin();
  if (!begun.ok()) {
    return begun.error();
  }
  auto task = SelectTaskForUpdate(lease.get(), options_, task_id);
  if (!task.ok()) {
    return task.error();
  }
  const auto dedupe_reason = FindHistoryDedupeReason(task.value().history(), message, policy);
  if (dedupe_reason.has_value()) {
    std::lock_guard<std::mutex> lock(telemetry_mutex_);
    UpdateDedupeSnapshot(&telemetry_snapshot_, *dedupe_reason);
  } else {
    *task.value().add_history() = message;
    auto updated = UpsertTask(lease.get(), options_, task.value());
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
  auto lease = pool_->Acquire();
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
}

PostgresPushNotificationStore::PostgresPushNotificationStore(std::shared_ptr<PostgresConnectionPool> pool,
                                                             PostgresStoreOptions options)
    : pool_(std::move(pool)), options_(std::move(options)) {
  ValidatePostgresStoreOptionsOrThrow(options_);
  auto lease = pool_->Acquire();
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
  PgResult result(PQexecParams(lease.get(), sql.c_str(), 4, nullptr, values, lengths, formats, 0));
  const auto checked = CheckCommand(lease.get(), result.get(), "upsert postgres push notification config");
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
  PgResult result(PQexecParams(lease.get(), sql.c_str(), 2, nullptr, values, nullptr, nullptr, 1));
  const auto checked = CheckTuples(lease.get(), result.get(), "get postgres push notification config");
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) == 0) {
    const std::string exists_sql = "SELECT 1 FROM " + PushTable(options_.schema) + " WHERE task_id = $1 LIMIT 1";
    PgResult exists(PQexecParams(lease.get(), exists_sql.c_str(), 1, nullptr, &values[0], nullptr, nullptr, 0));
    const auto exists_checked = CheckTuples(lease.get(), exists.get(), "check postgres push notification task configs");
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
  PgResult result(PQexecParams(lease.get(), sql.c_str(), 1, nullptr, values, nullptr, nullptr, 1));
  const auto checked = CheckTuples(lease.get(), result.get(), "list postgres push notification configs");
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
  PgResult result(PQexecParams(lease.get(), sql.c_str(), 2, nullptr, values, nullptr, nullptr, 0));
  return CheckCommand(lease.get(), result.get(), "delete postgres push notification config");
}

core::Result<StoreBundle> CreatePostgresStoreBundle(const PostgresStoreOptions& options) {
  const auto validation = ValidatePostgresStoreOptions(options);
  if (!validation.ok()) {
    return validation.error();
  }
  try {
    auto pool = MakePool(options);
    StoreBundle bundle;
    bundle.task_store = std::make_unique<PostgresTaskStore>(pool, options);
    bundle.push_store = std::make_unique<PostgresPushNotificationStore>(std::move(pool), options);
    return bundle;
  } catch (const std::exception& ex) {
    return core::Error::Internal(std::string("failed to create PostgreSQL store bundle: ") + ex.what());
  }
}

}  // namespace a2a::server::stores
