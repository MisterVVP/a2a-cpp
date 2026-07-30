// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/non_copyable.h"
#include "a2a/server/stores/store_factory.h"

typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;

namespace a2a::server::stores {

#ifdef A2A_POSTGRES_STORE_TESTING
void FailNextPostgresAcquireForTesting(core::Error error);

enum class PostgresDiagnosticPhase : std::size_t {
  kConnectionAcquireWait,
  kTaskUpsert,
  kPushConfigUpsert,
  kPushConfigList,
  kTransactionBegin,
  kTransactionCommit,
  kCount,
};

struct PostgresOperationDiagnostics final {
  std::array<double, static_cast<std::size_t>(PostgresDiagnosticPhase::kCount)> elapsed_ms{};
};

class PostgresDiagnosticTimerForTesting final {
 public:
  explicit PostgresDiagnosticTimerForTesting(PostgresDiagnosticPhase phase) noexcept;
  ~PostgresDiagnosticTimerForTesting();

 private:
  PostgresDiagnosticPhase phase_;
  std::chrono::steady_clock::time_point started_;
};

void ResetPostgresOperationDiagnosticsForTesting() noexcept;
[[nodiscard]] PostgresOperationDiagnostics TakePostgresOperationDiagnosticsForTesting() noexcept;
#endif

constexpr std::size_t kPostgresIdentifierMaxBytes = 63;
constexpr std::string_view kPublicSchema = "public";
constexpr std::string_view kTaskTableName = "a2a_tasks";
constexpr std::string_view kPushTableName = "a2a_push_notification_configs";
constexpr std::string_view kTaskCreatedSequenceName = "a2a_tasks_created_sequence";
constexpr std::string_view kPushCreatedSequenceName = "a2a_push_configs_created_sequence";
constexpr std::string_view kTasksCreatedSequenceIndex = "idx_a2a_tasks_created_sequence";
constexpr std::string_view kTasksContextIndex = "idx_a2a_tasks_context";
constexpr std::string_view kTasksStateIndex = "idx_a2a_tasks_state";
constexpr std::string_view kTasksCreatedSequenceIndexColumns = "(created_sequence ASC)";
constexpr std::string_view kTasksContextIndexColumns = "(context_id, created_sequence ASC)";
constexpr std::string_view kTasksStateIndexColumns = "(state, created_sequence ASC)";
constexpr std::string_view kPushConfigsTaskIndex = "idx_a2a_push_configs_task";
constexpr std::string_view kPushConfigsCreatedSequenceIndex = "idx_a2a_push_configs_created_sequence";
constexpr std::string_view kPushConfigsTaskIndexColumns = "(task_id)";
constexpr std::string_view kPushConfigsCreatedSequenceIndexColumns = "(task_id, created_sequence ASC)";

struct PgResultDeleter final {
  void operator()(PGresult* result) const noexcept;
};
using PgResult = std::unique_ptr<PGresult, PgResultDeleter>;

struct PgConnectionDeleter final {
  void operator()(PGconn* connection) const noexcept;
};
using PgConnection = std::unique_ptr<PGconn, PgConnectionDeleter>;

class PostgresConnectionPool final {
 public:
  explicit PostgresConnectionPool(std::string connection_string, std::size_t size = kDefaultPostgresConnectionPoolSize);

  class Lease final : private core::NonCopyable {
   public:
    Lease(PostgresConnectionPool* pool, PgConnection connection);
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept = delete;
    ~Lease();

    [[nodiscard]] PGconn* get() const noexcept;

   private:
    PostgresConnectionPool* pool_ = nullptr;
    PgConnection connection_;
  };

  [[nodiscard]] core::Result<Lease> Acquire();
  [[nodiscard]] std::size_t capacity() const noexcept;

 private:
  [[nodiscard]] core::Result<PgConnection> OpenConnection() const;
  void Return(PgConnection connection);

  std::string connection_string_;
  std::size_t capacity_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<PgConnection> connections_;
};

class Transaction final {
 public:
  explicit Transaction(PGconn* connection);
  [[nodiscard]] core::Result<void> Begin();
  [[nodiscard]] core::Result<void> Commit();
  ~Transaction();

 private:
  PGconn* connection_ = nullptr;
  bool committed_ = false;
};

[[nodiscard]] std::string TaskTable(std::string_view schema);
[[nodiscard]] std::string PushTable(std::string_view schema);
[[nodiscard]] core::Result<void> ValidatePostgresStoreOptions(const PostgresStoreOptions& options);
void ValidatePostgresStoreOptionsOrThrow(const PostgresStoreOptions& options);
[[nodiscard]] core::Result<void> CheckCommand(PGconn* connection, PGresult* result, std::string_view operation);
[[nodiscard]] core::Result<void> CheckTuples(PGconn* connection, PGresult* result, std::string_view operation);
[[nodiscard]] core::Result<void> Exec(PGconn* connection, const std::string& sql, std::string_view operation);
[[nodiscard]] core::Result<void> InitializeSchema(PGconn* connection, const PostgresStoreOptions& options);
[[nodiscard]] std::shared_ptr<PostgresConnectionPool> MakePool(const PostgresStoreOptions& options);
[[nodiscard]] PostgresConnectionPool::Lease AcquireOrThrow(PostgresConnectionPool& pool);

}  // namespace a2a::server::stores
