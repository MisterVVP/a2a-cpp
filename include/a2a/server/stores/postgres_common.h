// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
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
  kTaskGet,
  kTaskUpsert,
  kTaskHistoryLockRead,
  kPushConfigUpsert,
  kPushConfigGet,
  kPushConfigDelete,
  kPushConfigListCount,
  kPushConfigListSelect,
  kTransactionBegin,
  kTransactionCommit,
  kCount,
};

struct PostgresOperationDiagnostics final {
  std::array<double, static_cast<std::size_t>(PostgresDiagnosticPhase::kCount)> elapsed_ms{};
  std::array<std::size_t, static_cast<std::size_t>(PostgresDiagnosticPhase::kCount)> call_count{};
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

constexpr std::string_view kPostgresConnectionPoolSizeValidationMessage =
    "PostgreSQL connection_pool_size must be greater than zero";
constexpr std::string_view kPostgresConnectionPoolIdentityMismatchMessage =
    "PostgreSQL connection pool resolved to inconsistent logical target or role";
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
constexpr std::string_view kPushConfigsTaskForeignKey = "a2a_push_configs_task_fk";
constexpr std::string_view kDeleteTaskPushConfigsFunction = "a2a_delete_task_push_configs";
constexpr std::string_view kTaskPushConfigLockFunction = "a2a_lock_task_for_push_config";
constexpr std::string_view kTaskDeleteLockFunction = "a2a_lock_task_for_delete";
constexpr std::string_view kTaskPushConfigMigrationId = "task-aware-push-config-v3";
constexpr std::string_view kDeleteTaskPushConfigsTrigger = "a2a_delete_task_push_configs_trigger";
constexpr std::string_view kTaskDeleteLockTrigger = "a2a_lock_task_for_delete_trigger";
constexpr std::size_t kDeleteTaskPushConfigsFunctionSqlReserveSlack = 160U;
constexpr std::size_t kDeleteTaskPushConfigsTriggerSqlReserveSlack = 192U;
constexpr std::size_t kRevokeDeleteTaskPushConfigsFunctionSqlReserveSlack = 48U;

struct PostgresStorageCoordinates final {
  std::string host;
  std::string host_address;
  std::string port;
  std::string database;
  std::string target_session_attributes;
  std::string schema;
  std::string storage_authority_id = {};

  friend bool operator==(const PostgresStorageCoordinates&, const PostgresStorageCoordinates&) = default;
};

// Pool connection identity is for detecting drift within one pool. It must not
// be used to prove that independent pools have equivalent RLS/session context.
struct PostgresExecutionIdentity final {
  PostgresStorageCoordinates storage;
  std::string effective_role;

  friend bool operator==(const PostgresExecutionIdentity&, const PostgresExecutionIdentity&) = default;
};

enum class PostgresStorageAuthority : std::uint8_t {
  kLocal,
  kExternal,
  kUncertain,
};

#ifdef A2A_POSTGRES_STORE_TESTING
void OverrideNextPostgresConnectionIdentityForTesting(PostgresExecutionIdentity identity);
void ClearPostgresConnectionIdentityOverrideForTesting();
#endif

using PostgresStorageIdentity = PostgresStorageCoordinates;

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
  [[nodiscard]] PostgresStorageCoordinates StorageCoordinates(std::string schema,
                                                              std::string_view storage_authority_id = {}) const;
  [[nodiscard]] PostgresStorageIdentity StorageIdentity(std::string schema,
                                                        std::string_view storage_authority_id = {}) const {
    return StorageCoordinates(std::move(schema), storage_authority_id);
  }
  [[nodiscard]] PostgresExecutionIdentity ExecutionIdentity(std::string schema,
                                                            std::string_view storage_authority_id = {}) const;

 private:
  [[nodiscard]] core::Result<PgConnection> OpenConnection() const;
  void Return(PgConnection connection);

  std::string connection_string_;
  PostgresExecutionIdentity database_identity_;
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
[[nodiscard]] std::string TaskPushConfigLockFunction(std::string_view schema);
[[nodiscard]] std::string TaskDeleteLockFunction(std::string_view schema);
[[nodiscard]] std::string ExpectedTaskPushConfigLockFunctionBody(std::string_view schema);
[[nodiscard]] std::string ExpectedTaskDeleteLockFunctionBody(std::string_view schema);
[[nodiscard]] std::string ExpectedDeleteTaskPushConfigsFunctionBody(std::string_view schema);
[[nodiscard]] PostgresStorageAuthority ClassifyPostgresStorageAuthority(const PostgresStorageIdentity& lhs,
                                                                        const PostgresStorageIdentity& rhs) noexcept;
[[nodiscard]] core::Result<void> ValidatePostgresStoreOptions(const PostgresStoreOptions& options);
void ValidatePostgresStoreOptionsOrThrow(const PostgresStoreOptions& options);
[[nodiscard]] core::Result<void> CheckCommand(PGconn* connection, PGresult* result, std::string_view operation);
[[nodiscard]] core::Result<void> CheckTuples(PGconn* connection, PGresult* result, std::string_view operation);
[[nodiscard]] core::Result<void> Exec(PGconn* connection, const std::string& sql, std::string_view operation);
[[nodiscard]] core::Result<void> InitializeSchema(PGconn* connection, const PostgresStoreOptions& options);
[[nodiscard]] std::shared_ptr<PostgresConnectionPool> MakePool(const PostgresStoreOptions& options);
[[nodiscard]] PostgresConnectionPool::Lease AcquireOrThrow(PostgresConnectionPool& pool);

}  // namespace a2a::server::stores
