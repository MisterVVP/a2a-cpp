// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/stores/postgres_task_store.h"

#include <libpq-fe.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
constexpr std::size_t kConditionalTaskWriteSqlReserve = 300U;
constexpr std::size_t kTaskHistoryUpdateSqlReserve = 120U;
constexpr std::string_view kNoRowsAffected = "0";

[[nodiscard]] std::string BuildTaskSnapshotSql(std::string_view schema) {
  std::string sql = "SELECT task_proto, revision::text FROM ";
  const std::string table = TaskTable(schema);
  sql.reserve(sql.size() + table.size() + kTaskSnapshotSqlReserve);
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

[[nodiscard]] core::Result<std::uint64_t> UpsertTask(PGconn* connection, const PostgresStoreOptions& options,
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
      "revision = target.revision + 1, updated_at = now() RETURNING revision");
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
  const auto checked = CheckTuples(connection, result.get(), "upsert postgres task");
  if (!checked.ok()) {
    return checked.error();
  }
  std::uint64_t revision = 0;
  const std::string_view raw_revision(PQgetvalue(result.get(), 0, 0));
  const auto parsed = std::from_chars(raw_revision.data(), raw_revision.data() + raw_revision.size(), revision);
  if (parsed.ec != std::errc{} || parsed.ptr != raw_revision.data() + raw_revision.size()) {
    return core::Error::Serialization("failed to parse upserted postgres task revision");
  }
  return revision;
}

[[nodiscard]] core::Result<bool> UpdateTaskHistory(PGconn* connection, const PostgresStoreOptions& options,
                                                   const lf::a2a::v1::Task& task, std::uint64_t expected_revision) {
  const std::string payload = task.SerializeAsString();
  const std::string revision = std::to_string(expected_revision);
  const std::string table = TaskTable(options.schema);
  std::string sql;
  sql.reserve(table.size() + kTaskHistoryUpdateSqlReserve);
  sql.append("UPDATE ");
  sql.append(table);
  sql.append(" SET task_proto = $2, revision = revision + 1, updated_at = now() WHERE id = $1 AND revision = $3");
  constexpr int kTaskHistoryUpdateParameterCount = 3;
  const std::array<const char*, kTaskHistoryUpdateParameterCount> values = {task.id().c_str(), payload.data(),
                                                                            revision.c_str()};
  const std::array<int, kTaskHistoryUpdateParameterCount> lengths = {0, static_cast<int>(payload.size()), 0};
  const std::array<int, kTaskHistoryUpdateParameterCount> formats = {0, 1, 0};
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
};

[[nodiscard]] core::Result<lf::a2a::v1::Task> ParseTaskRow(PGresult* result, int row);

[[nodiscard]] core::Result<TaskHistorySnapshot> SelectTaskHistorySnapshot(PGconn* connection,
                                                                          const PostgresStoreOptions& options,
                                                                          std::string_view id) {
  const std::string id_value(id);
  const std::string sql = "SELECT task_proto, revision::text FROM " + TaskTable(options.schema) + " WHERE id = $1";
  const std::array<const char*, 1> values = {id_value.c_str()};
  PgResult result;
#ifdef A2A_POSTGRES_STORE_TESTING
  {
    const PostgresDiagnosticTimerForTesting timer(PostgresDiagnosticPhase::kTaskHistoryLockRead);
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
  std::uint64_t revision = 0;
  const std::string_view raw_revision(PQgetvalue(result.get(), 0, 1));
  const auto parsed = std::from_chars(raw_revision.data(), raw_revision.data() + raw_revision.size(), revision);
  if (parsed.ec != std::errc() || parsed.ptr != raw_revision.data() + raw_revision.size()) {
    return core::Error::Serialization("failed to parse postgres task history revision");
  }
  return TaskHistorySnapshot{.task = std::move(task.value()), .revision = revision};
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

class PostgresTaskStore::MutationSnapshotCache final {
 public:
  explicit MutationSnapshotCache(std::size_t capacity) : capacity_per_shard_(capacity / kShardCount) {}

  [[nodiscard]] std::optional<TaskSnapshot> Find(std::string_view task_id) const {
    Shard& shard = shards_[ShardIndex(task_id)];
    std::lock_guard lock(shard.mutex);
    const auto found = shard.entries.find(std::string(task_id));
    if (found == shard.entries.end()) {
      misses_.fetch_add(1U, std::memory_order_relaxed);
      if (shard.conflict_invalidations.erase(std::string(task_id)) != 0U) {
        authoritative_reloads_.fetch_add(1U, std::memory_order_relaxed);
      }
      return std::nullopt;
    }
    shard.recency.splice(shard.recency.begin(), shard.recency, found->second.recency);
    hits_.fetch_add(1U, std::memory_order_relaxed);
    return found->second.snapshot;
  }

  void Put(const TaskSnapshot& snapshot) {
    if (capacity_per_shard_ == 0U) {
      return;
    }
    Shard& shard = shards_[ShardIndex(snapshot.task.id())];
    std::lock_guard lock(shard.mutex);
    auto found = shard.entries.find(snapshot.task.id());
    if (found != shard.entries.end()) {
      found->second.snapshot = snapshot;
      shard.recency.splice(shard.recency.begin(), shard.recency, found->second.recency);
      return;
    }
    shard.recency.push_front(snapshot.task.id());
    shard.entries.emplace(snapshot.task.id(), Entry{.snapshot = snapshot, .recency = shard.recency.begin()});
    if (shard.entries.size() > capacity_per_shard_) {
      shard.entries.erase(shard.recency.back());
      shard.recency.pop_back();
    }
  }

  void Invalidate(std::string_view task_id) {
    Shard& shard = shards_[ShardIndex(task_id)];
    std::lock_guard lock(shard.mutex);
    const auto found = shard.entries.find(std::string(task_id));
    if (found != shard.entries.end()) {
      shard.recency.erase(found->second.recency);
      shard.entries.erase(found);
    }
    if (shard.conflict_invalidations.size() >= capacity_per_shard_) {
      shard.conflict_invalidations.clear();
    }
    shard.conflict_invalidations.emplace(task_id);
    conflict_invalidations_.fetch_add(1U, std::memory_order_relaxed);
  }

  [[nodiscard]] MutationCacheTelemetrySnapshot Telemetry() const noexcept {
    return {.hits = hits_.load(std::memory_order_relaxed),
            .misses = misses_.load(std::memory_order_relaxed),
            .conflict_invalidations = conflict_invalidations_.load(std::memory_order_relaxed),
            .authoritative_reloads = authoritative_reloads_.load(std::memory_order_relaxed)};
  }

 private:
  static constexpr std::size_t kShardCount = 16U;

  struct Entry final {
    TaskSnapshot snapshot;
    std::list<std::string>::iterator recency;
  };

  struct Shard final {
    mutable std::mutex mutex;
    std::unordered_map<std::string, Entry> entries;
    std::list<std::string> recency;
    std::unordered_set<std::string> conflict_invalidations;
  };

  [[nodiscard]] static std::size_t ShardIndex(std::string_view task_id) {
    return std::hash<std::string_view>{}(task_id) % kShardCount;
  }

  std::size_t capacity_per_shard_;
  mutable std::array<Shard, kShardCount> shards_;
  mutable std::atomic<std::size_t> hits_{0U};
  mutable std::atomic<std::size_t> misses_{0U};
  std::atomic<std::size_t> conflict_invalidations_{0U};
  mutable std::atomic<std::size_t> authoritative_reloads_{0U};
};

class PostgresTaskStore::ConditionalWriteBatcher final {
 public:
  ConditionalWriteBatcher(PostgresTaskStore* store, std::size_t maximum_batch_size,
                          std::chrono::microseconds maximum_delay, bool generated_creates = false)
      : store_(store),
        maximum_batch_size_(maximum_batch_size),
        maximum_delay_(maximum_delay),
        generated_creates_(generated_creates),
        worker_([this] { Run(); }) {}

  ~ConditionalWriteBatcher() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_one();
    worker_.join();
  }

  [[nodiscard]] core::Result<ConditionalWriteResult> Submit(const lf::a2a::v1::Task& task,
                                                            std::uint64_t expected_revision) {
    const std::size_t active_requests = active_requests_.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (active_requests < kMinimumBatchConcurrency) {
      auto result = store_->PersistConditionalWrite(task, expected_revision);
      active_requests_.fetch_sub(1U, std::memory_order_relaxed);
      return result;
    }
    auto request = std::make_shared<Request>(task, expected_revision);
    auto future = request->result.get_future();
    {
      std::lock_guard lock(mutex_);
      queue_.push_back(request);
    }
    condition_.notify_one();
    auto result = future.get();
    active_requests_.fetch_sub(1U, std::memory_order_relaxed);
    return result;
  }

  [[nodiscard]] ConditionalBatchTelemetrySnapshot Telemetry() const noexcept {
    ConditionalBatchTelemetrySnapshot snapshot;
    for (std::size_t index = 0; index < snapshot.batches_by_size.size(); ++index) {
      snapshot.batches_by_size[index] = batches_by_size_[index].load(std::memory_order_relaxed);
    }
    snapshot.queued_nanoseconds = queued_nanoseconds_.load(std::memory_order_relaxed);
    snapshot.queued_writes = queued_writes_.load(std::memory_order_relaxed);
    return snapshot;
  }

 private:
  static constexpr std::size_t kParametersPerWrite = 8U;
  static constexpr std::size_t kCreateParametersPerWrite = 7U;
  static constexpr std::size_t kMinimumBatchConcurrency = 5U;

  struct Request final {
    Request(lf::a2a::v1::Task input_task, std::uint64_t input_revision)
        : task(std::move(input_task)),
          expected_revision(input_revision),
          enqueued_at(std::chrono::steady_clock::now()) {}
    lf::a2a::v1::Task task;
    std::uint64_t expected_revision;
    std::promise<core::Result<ConditionalWriteResult>> result;
    std::chrono::steady_clock::time_point enqueued_at;
  };

  void Run() {
    for (;;) {
      std::vector<std::shared_ptr<Request>> batch;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) {
          return;
        }
        if (active_requests_.load(std::memory_order_relaxed) >= kMinimumBatchConcurrency) {
          condition_.wait_for(lock, maximum_delay_,
                              [this] { return stopping_ || queue_.size() >= maximum_batch_size_; });
        }
        batch = DrainBatch();
      }
      Persist(batch);
    }
  }

  [[nodiscard]] std::vector<std::shared_ptr<Request>> DrainBatch() {
    std::vector<std::shared_ptr<Request>> batch;
    const std::size_t batch_limit =
        active_requests_.load(std::memory_order_relaxed) >= kMinimumBatchConcurrency ? maximum_batch_size_ : 1U;
    batch.reserve(batch_limit);
    std::unordered_set<std::string_view> task_ids;
    for (auto request = queue_.begin(); request != queue_.end() && batch.size() < batch_limit;) {
      if (task_ids.insert((*request)->task.id()).second) {
        batch.push_back(std::move(*request));
        request = queue_.erase(request);
      } else {
        ++request;
      }
    }
    return batch;
  }

  void Persist(const std::vector<std::shared_ptr<Request>>& batch) {
    batches_by_size_[batch.size()].fetch_add(1U, std::memory_order_relaxed);
    const auto started = std::chrono::steady_clock::now();
    for (const auto& request : batch) {
      const auto queued = std::chrono::duration_cast<std::chrono::nanoseconds>(started - request->enqueued_at).count();
      queued_nanoseconds_.fetch_add(static_cast<std::uint64_t>(queued), std::memory_order_relaxed);
      queued_writes_.fetch_add(1U, std::memory_order_relaxed);
    }
    if (batch.size() == 1U) {
      const auto& request = batch.front();
      request->result.set_value(store_->PersistConditionalWrite(request->task, request->expected_revision));
      return;
    }
    const auto persisted = PersistBatch(batch);
    if (!persisted.ok()) {
      for (const auto& request : batch) {
        request->result.set_value(store_->PersistConditionalWrite(request->task, request->expected_revision));
      }
      return;
    }
    for (const auto& request : batch) {
      const bool updated = persisted.value().contains(request->task.id());
      if (store_->mutation_cache_ != nullptr && !generated_creates_) {
        if (updated) {
          store_->mutation_cache_->Put(
              TaskSnapshot{.task = request->task, .revision = request->expected_revision + 1U});
        } else {
          store_->mutation_cache_->Invalidate(request->task.id());
        }
      }
      request->result.set_value(updated ? ConditionalWriteResult::kUpdated : ConditionalWriteResult::kConflict);
    }
  }

  [[nodiscard]] core::Result<std::unordered_set<std::string>> PersistBatch(
      const std::vector<std::shared_ptr<Request>>& batch) {
    std::vector<std::string> parameters;
    std::vector<int> lengths;
    std::vector<int> formats;
    parameters.reserve(batch.size() * (generated_creates_ ? kCreateParametersPerWrite : kParametersPerWrite));
    lengths.reserve(parameters.capacity());
    formats.reserve(parameters.capacity());
    std::string sql = BuildSql(batch, &parameters, &lengths, &formats);
    const auto values = BuildSqlParameterPointers(parameters);
    auto lease = store_->pool_->Acquire();
    if (!lease.ok()) {
      return lease.error();
    }
    PgResult result(PQexecParams(lease.value().get(), sql.c_str(), static_cast<int>(values.size()), nullptr,
                                 values.data(), lengths.data(), formats.data(), 0));
    const auto checked = CheckTuples(lease.value().get(), result.get(), "conditionally persist postgres task batch");
    if (!checked.ok()) {
      return checked.error();
    }
    std::unordered_set<std::string> updated_task_ids;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
      updated_task_ids.emplace(PQgetvalue(result.get(), row, 0));
    }
    return updated_task_ids;
  }

  [[nodiscard]] std::string BuildSql(const std::vector<std::shared_ptr<Request>>& batch,
                                     std::vector<std::string>* parameters, std::vector<int>* lengths,
                                     std::vector<int>* formats) const {
    if (generated_creates_) {
      return BuildCreateSql(batch, parameters, lengths, formats);
    }
    std::string sql = "UPDATE ";
    sql.append(TaskTable(store_->options_.schema));
    sql.append(
        " AS task SET context_id = input.context_id, state = input.state, "
        "has_status_timestamp = input.has_status_timestamp, status_seconds = input.status_seconds, "
        "status_nanos = input.status_nanos, task_proto = input.task_proto, revision = task.revision + 1, "
        "updated_at = now() FROM (VALUES ");
    for (std::size_t index = 0; index < batch.size(); ++index) {
      if (index != 0U) {
        sql.append(", ");
      }
      AppendInput(&sql, parameters, lengths, formats, *batch[index]);
    }
    sql.append(
        ") AS input(id, context_id, state, has_status_timestamp, status_seconds, status_nanos, task_proto, "
        "expected_revision) WHERE task.id = input.id AND task.revision = input.expected_revision RETURNING task.id");
    return sql;
  }

  [[nodiscard]] std::string BuildCreateSql(const std::vector<std::shared_ptr<Request>>& batch,
                                           std::vector<std::string>* parameters, std::vector<int>* lengths,
                                           std::vector<int>* formats) const {
    std::string sql = "INSERT INTO ";
    sql.append(TaskTable(store_->options_.schema));
    sql.append(" (id, context_id, state, has_status_timestamp, status_seconds, status_nanos, task_proto) VALUES ");
    for (std::size_t index = 0; index < batch.size(); ++index) {
      if (index != 0U) {
        sql.append(", ");
      }
      AppendCreateInput(&sql, parameters, lengths, formats, *batch[index]);
    }
    sql.append(" ON CONFLICT (id) DO NOTHING RETURNING id");
    return sql;
  }

  static void AppendInput(std::string* sql, std::vector<std::string>* parameters, std::vector<int>* lengths,
                          std::vector<int>* formats, const Request& request) {
    const bool has_timestamp = request.task.status().has_timestamp();
    parameters->push_back(request.task.id());
    parameters->push_back(request.task.context_id());
    parameters->push_back(std::to_string(static_cast<int>(request.task.status().state())));
    parameters->emplace_back(has_timestamp ? "true" : "false");
    parameters->push_back(std::to_string(has_timestamp ? request.task.status().timestamp().seconds() : 0));
    parameters->push_back(std::to_string(has_timestamp ? request.task.status().timestamp().nanos() : 0));
    parameters->push_back(request.task.SerializeAsString());
    parameters->push_back(std::to_string(request.expected_revision));
    constexpr std::array<std::string_view, kParametersPerWrite> kCasts = {
        "::text", "::text", "::integer", "::boolean", "::bigint", "::integer", "::bytea", "::bigint"};
    const std::size_t first = parameters->size() - kParametersPerWrite;
    sql->push_back('(');
    for (std::size_t offset = 0; offset < kParametersPerWrite; ++offset) {
      if (offset != 0U) {
        sql->append(", ");
      }
      sql->push_back('$');
      sql->append(std::to_string(first + offset + 1U));
      sql->append(kCasts[offset]);
      const bool binary = offset == 6U;
      lengths->push_back(binary ? static_cast<int>((*parameters)[first + offset].size()) : 0);
      formats->push_back(binary ? 1 : 0);
    }
    sql->push_back(')');
  }

  static void AppendCreateInput(std::string* sql, std::vector<std::string>* parameters, std::vector<int>* lengths,
                                std::vector<int>* formats, const Request& request) {
    const bool has_timestamp = request.task.status().has_timestamp();
    parameters->push_back(request.task.id());
    parameters->push_back(request.task.context_id());
    parameters->push_back(std::to_string(static_cast<int>(request.task.status().state())));
    parameters->emplace_back(has_timestamp ? "true" : "false");
    parameters->push_back(std::to_string(has_timestamp ? request.task.status().timestamp().seconds() : 0));
    parameters->push_back(std::to_string(has_timestamp ? request.task.status().timestamp().nanos() : 0));
    parameters->push_back(request.task.SerializeAsString());
    const std::size_t first = parameters->size() - kCreateParametersPerWrite;
    sql->push_back('(');
    for (std::size_t offset = 0; offset < kCreateParametersPerWrite; ++offset) {
      if (offset != 0U) {
        sql->append(", ");
      }
      sql->push_back('$');
      sql->append(std::to_string(first + offset + 1U));
      const bool binary = offset == 6U;
      lengths->push_back(binary ? static_cast<int>((*parameters)[first + offset].size()) : 0);
      formats->push_back(binary ? 1 : 0);
    }
    sql->push_back(')');
  }

  PostgresTaskStore* store_;
  std::size_t maximum_batch_size_;
  std::chrono::microseconds maximum_delay_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<std::shared_ptr<Request>> queue_;
  bool stopping_ = false;
  bool generated_creates_;
  std::atomic<std::size_t> active_requests_{0U};
  static constexpr std::size_t kBatchSizeBucketCount = 9U;
  std::array<std::atomic<std::size_t>, kBatchSizeBucketCount> batches_by_size_{};
  std::atomic<std::uint64_t> queued_nanoseconds_{0U};
  std::atomic<std::size_t> queued_writes_{0U};
  std::thread worker_;
};

PostgresTaskStore::PostgresTaskStore(PostgresStoreOptions options)
    : pool_(MakePool(options)),
      options_(std::move(options)),
      storage_identity_(pool_->StorageCoordinates(options_.schema, options_.storage_authority_id)),
      execution_identity_(pool_->ExecutionIdentity(options_.schema, options_.storage_authority_id)),
      snapshot_sql_(BuildTaskSnapshotSql(options_.schema)),
      conditional_create_sql_(BuildConditionalTaskCreateSql(options_.schema)),
      conditional_update_sql_(BuildConditionalTaskUpdateSql(options_.schema)) {
  auto lease = AcquireOrThrow(*pool_);
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
  if (options_.mutation_cache_capacity != 0U) {
    mutation_cache_ = std::make_unique<MutationSnapshotCache>(options_.mutation_cache_capacity);
  }
  if (options_.conditional_write_batch_size > 1U) {
    conditional_write_batcher_ = std::make_unique<ConditionalWriteBatcher>(
        this, options_.conditional_write_batch_size,
        std::chrono::microseconds(options_.conditional_write_batch_delay_microseconds));
  }
  if (options_.generated_task_batch_size > 1U) {
    generated_task_batcher_ = std::make_unique<ConditionalWriteBatcher>(
        this, options_.generated_task_batch_size,
        std::chrono::microseconds(options_.conditional_write_batch_delay_microseconds), true);
  }
}

PostgresTaskStore::PostgresTaskStore(std::shared_ptr<PostgresConnectionPool> pool, PostgresStoreOptions options)
    : pool_(std::move(pool)),
      options_(std::move(options)),
      storage_identity_(pool_->StorageCoordinates(options_.schema, options_.storage_authority_id)),
      execution_identity_(pool_->ExecutionIdentity(options_.schema, options_.storage_authority_id)),
      snapshot_sql_(BuildTaskSnapshotSql(options_.schema)),
      conditional_create_sql_(BuildConditionalTaskCreateSql(options_.schema)),
      conditional_update_sql_(BuildConditionalTaskUpdateSql(options_.schema)) {
  ValidatePostgresStoreOptionsOrThrow(options_);
  auto lease = AcquireOrThrow(*pool_);
  const auto initialized = InitializeSchema(lease.get(), options_);
  if (!initialized.ok()) {
    throw std::runtime_error(std::string(initialized.error().message()));
  }
  if (options_.mutation_cache_capacity != 0U) {
    mutation_cache_ = std::make_unique<MutationSnapshotCache>(options_.mutation_cache_capacity);
  }
  if (options_.conditional_write_batch_size > 1U) {
    conditional_write_batcher_ = std::make_unique<ConditionalWriteBatcher>(
        this, options_.conditional_write_batch_size,
        std::chrono::microseconds(options_.conditional_write_batch_delay_microseconds));
  }
  if (options_.generated_task_batch_size > 1U) {
    generated_task_batcher_ = std::make_unique<ConditionalWriteBatcher>(
        this, options_.generated_task_batch_size,
        std::chrono::microseconds(options_.conditional_write_batch_delay_microseconds), true);
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
  auto upserted = UpsertTask(lease.value().get(), options_, task);
  if (!upserted.ok()) {
    return upserted.error();
  }
  if (mutation_cache_ != nullptr) {
    mutation_cache_->Put(TaskSnapshot{.task = task, .revision = upserted.value()});
  }
  return {};
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

core::Result<TaskStore::TaskSnapshot> PostgresTaskStore::GetMutationSnapshot(std::string_view id) const {
  if (mutation_cache_ != nullptr) {
    auto cached = mutation_cache_->Find(id);
    if (cached.has_value()) {
      return std::move(*cached);
    }
  }
  auto snapshot = GetSnapshot(id);
  if (snapshot.ok() && mutation_cache_ != nullptr) {
    mutation_cache_->Put(snapshot.value());
  }
  return snapshot;
}

core::Result<TaskStore::ConditionalWriteResult> PostgresTaskStore::CreateOrUpdateIfRevision(
    const lf::a2a::v1::Task& task, std::uint64_t expected_revision) {
  if (task.id().empty()) {
    return core::Error::Validation(std::string(kTaskIdFieldRequiredMessage));
  }
  if (expected_revision != 0U && conditional_write_batcher_ != nullptr) {
    return conditional_write_batcher_->Submit(task, expected_revision);
  }
  return PersistConditionalWrite(task, expected_revision);
}

core::Result<TaskStore::ConditionalWriteResult> PostgresTaskStore::PersistConditionalWrite(
    const lf::a2a::v1::Task& task, std::uint64_t expected_revision) {
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
  if (PQntuples(result.get()) == 0) {
    if (mutation_cache_ != nullptr) {
      mutation_cache_->Invalidate(task.id());
    }
    return ConditionalWriteResult::kConflict;
  }
  if (mutation_cache_ != nullptr && expected_revision != 0U) {
    mutation_cache_->Put(TaskSnapshot{.task = task, .revision = expected_revision + 1U});
  }
  return ConditionalWriteResult::kUpdated;
}

core::Result<TaskStore::ConditionalWriteResult> PostgresTaskStore::CreateGeneratedTaskIfAbsent(
    const lf::a2a::v1::Task& task) {
  if (task.id().empty()) {
    return core::Error::Validation(std::string(kTaskIdFieldRequiredMessage));
  }
  return generated_task_batcher_ == nullptr ? PersistConditionalWrite(task, 0U)
                                            : generated_task_batcher_->Submit(task, 0U);
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
  if (mutation_cache_ != nullptr) {
    mutation_cache_->Put(TaskSnapshot{.task = task.value(), .revision = updated.value()});
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
  for (;;) {
    auto snapshot = SelectTaskHistorySnapshot(lease.value().get(), options_, task_id);
    if (!snapshot.ok()) {
      return snapshot.error();
    }
    const auto dedupe_reason = FindHistoryDedupeReason(snapshot.value().task.history(), message, policy);
    if (dedupe_reason.has_value()) {
      std::lock_guard<std::mutex> lock(telemetry_mutex_);
      UpdateDedupeSnapshot(&telemetry_snapshot_, *dedupe_reason);
      return snapshot.value().task;
    }
    *snapshot.value().task.add_history() = message;
    auto updated = UpdateTaskHistory(lease.value().get(), options_, snapshot.value().task, snapshot.value().revision);
    if (!updated.ok()) {
      return updated.error();
    }
    if (updated.value()) {
      if (mutation_cache_ != nullptr) {
        mutation_cache_->Put(TaskSnapshot{.task = snapshot.value().task, .revision = snapshot.value().revision + 1U});
      }
      return snapshot.value().task;
    }
  }
}

TaskStore::HistoryTelemetrySnapshot PostgresTaskStore::GetHistoryTelemetrySnapshot() const {
  std::lock_guard<std::mutex> lock(telemetry_mutex_);
  return telemetry_snapshot_;
}

TaskStore::MutationCacheTelemetrySnapshot PostgresTaskStore::GetMutationCacheTelemetrySnapshot() const {
  return mutation_cache_ == nullptr ? MutationCacheTelemetrySnapshot{} : mutation_cache_->Telemetry();
}

TaskStore::ConditionalBatchTelemetrySnapshot PostgresTaskStore::GetConditionalBatchTelemetrySnapshot() const {
  ConditionalBatchTelemetrySnapshot combined;
  for (const ConditionalWriteBatcher* batcher : {conditional_write_batcher_.get(), generated_task_batcher_.get()}) {
    if (batcher == nullptr) {
      continue;
    }
    const auto telemetry = batcher->Telemetry();
    for (std::size_t index = 0; index < combined.batches_by_size.size(); ++index) {
      combined.batches_by_size[index] += telemetry.batches_by_size[index];
    }
    combined.queued_nanoseconds += telemetry.queued_nanoseconds;
    combined.queued_writes += telemetry.queued_writes;
  }
  return combined;
}

}  // namespace a2a::server::stores
