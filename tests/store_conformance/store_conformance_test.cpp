// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <array>
#include <atomic>
#include <barrier>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/server/push_notification_store.h"
#include "a2a/server/stores/store_factory.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_store.h"
#include "store_conformance/push_notification_store_conformance.h"
#include "store_conformance/task_store_conformance.h"

#ifdef A2A_ENABLE_POSTGRES_STORE
#include <libpq-fe.h>

#include "a2a/server/stores/postgres_common.h"
#include "a2a/server/stores/postgres_notification_store.h"
#include "a2a/server/stores/postgres_task_store.h"
#include "a2a/server/stores/sql_identifier.h"
#endif

namespace {

TEST(StoreConformanceTest, InMemoryTaskStore) {
  a2a::tests::store_conformance::RunTaskStoreConformance(
      [] { return std::make_unique<a2a::server::InMemoryTaskStore>(); });
}

TEST(StoreConformanceTest, InMemoryPushNotificationStore) {
  a2a::tests::store_conformance::RunPushNotificationStoreConformance(
      [] { return std::make_unique<a2a::server::InMemoryPushNotificationStore>(); });
}

#ifdef A2A_ENABLE_POSTGRES_STORE
[[nodiscard]] const char* GetPostgresDsn() { return std::getenv("A2A_TEST_POSTGRES_DSN"); }

[[nodiscard]] a2a::core::Error MakePostgresAcquireFailureForTesting() {
  return a2a::core::Error::Internal("test postgres acquire failure");
}

void ExpectPostgresAcquireFailure(const a2a::core::Error& error) {
  EXPECT_EQ(error.code(), a2a::core::ErrorCode::kInternal);
  EXPECT_NE(error.message().find("test postgres acquire failure"), std::string_view::npos);
}

[[nodiscard]] std::string MakePostgresTestSchema(std::string_view suffix) {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return "a2a_test_" + std::to_string(ticks) + "_" + std::string(suffix);
}

constexpr std::string_view kTargetContext = "target-context";
constexpr std::string_view kOtherContext = "other-context";
constexpr std::string_view kOldTargetTaskId = "old-target-context-task";
constexpr std::string_view kNewTargetTaskId = "new-target-context-task";
constexpr std::string_view kOtherContextTaskId = "other-context-task";
constexpr std::string_view kCompletedTargetTaskId = "completed-target-context-task";
constexpr std::size_t kFilteredTaskCount = 2;
constexpr std::size_t kSingleTaskPageSize = 1;
constexpr int kOldTargetTaskTimestampSeconds = 1000;
constexpr int kNewTargetTaskTimestampSeconds = 3000;
constexpr int kOtherContextTaskTimestampSeconds = 4000;
constexpr int kCompletedTargetTaskTimestampSeconds = 5000;
constexpr std::size_t kConcurrentPageSize = 1;
constexpr std::array<std::string_view, 4> kConcurrentIds = {"connection-a-first", "connection-b-first",
                                                            "connection-a-second", "connection-b-second"};
constexpr std::string_view kListEdgeTaskId = "push-list-edge-task";
constexpr std::string_view kEmptyListTaskId = "push-list-empty-task";
constexpr std::string_view kMissingListTaskId = "push-list-missing-task";
constexpr std::string_view kFirstListConfigId = "push-list-first-config";
constexpr std::string_view kSecondListConfigId = "push-list-second-config";
constexpr std::string_view kPushListContextId = "push-list-context";
constexpr std::string_view kTotalConfigCountToken = "2";
constexpr std::string_view kOutOfRangeConfigToken = "3";
constexpr std::string_view kBeyondPostgresBigintToken = "9223372036854775808";
constexpr std::string_view kFirstPageToken = "1";
constexpr int kBoundedPushListPageSize = 1;
constexpr int kPushListConfigCount = 2;
constexpr std::string_view kAtomicCreateTaskId = "atomic-create-task";
constexpr std::string_view kAtomicCreateConfigId = "atomic-create-config";
constexpr std::string_view kMixedCreateTaskId = "mixed-create-task";
constexpr std::string_view kMixedCreateConfigId = "mixed-create-config";
constexpr std::string_view kHoldConcurrentDeleteOperation = "hold concurrent task deletion";
constexpr auto kConcurrentCreateBlockedTimeout = std::chrono::milliseconds(50);
constexpr std::size_t kLargeConfigMetadataSize = std::size_t{64U} * 1024U;
constexpr char kLargeConfigMetadataFill = 'x';
constexpr std::string_view kLargeConfigAuthScheme = "Bearer";
constexpr std::string_view kLargeConfigUpdatedUrl = "https://example.test/updated-large";
constexpr std::string_view kDefaultPostgresPort = "5432";
constexpr std::string_view kConninfoPassword = "password";
constexpr std::string_view kConninfoDatabase = "dbname";
constexpr std::string_view kConninfoHost = "host";
constexpr std::string_view kConninfoUser = "user";
constexpr std::string_view kConninfoPort = "port";
constexpr std::string_view kConninfoApplicationName = "application_name";
constexpr std::string_view kIdentityApplicationName = "a2a-identity-test";
constexpr std::string_view kUriScheme = "postgresql://";
constexpr std::string_view kHexDigits = "0123456789ABCDEF";
constexpr unsigned char kLowNibbleMask = 0x0FU;
constexpr std::string_view kMaintenanceRolePrefix = "a2a_push_maintenance_";
constexpr std::string_view kRoleSetupOperation = "set up least-privilege maintenance role";
constexpr std::string_view kRoleSwitchOperation = "switch to least-privilege maintenance role";
constexpr std::string_view kRoleResetOperation = "reset least-privilege maintenance role";
constexpr std::string_view kRoleCleanupOperation = "clean up least-privilege maintenance role";
constexpr std::string_view kRoleTaskDeleteOperation = "delete task as least-privilege maintenance role";
constexpr std::string_view kRolePushDeleteOperation = "directly delete push config as maintenance role";
constexpr std::string_view kPushTaskIdColumn = "task_id";
constexpr std::string_view kResetRoleSql = "RESET ROLE";
constexpr std::string_view kSecretDsn =
    "host=invalid.invalid dbname=a2a user=a2a password=storage-identity-secret connect_timeout=1";
constexpr std::string_view kSecretDsnPassword = "storage-identity-secret";
constexpr std::string_view kUnexpectedSecretDsnConnection = "invalid PostgreSQL endpoint unexpectedly connected";
constexpr std::string_view kIdentitySchema = "identity_schema";
constexpr std::string_view kDefaultPortIdentitySchema = "default_port_identity_schema";
constexpr std::string_view kNonDefaultPortSkipMessage = "PostgreSQL test DSN does not use the default port";
constexpr char kPgPortEnvironmentVariable[] = "PGPORT";
constexpr std::string_view kNonDefaultPgPortSkipMessage = "PGPORT overrides the libpq default port";
constexpr std::string_view kSplitRolePrefix = "a2a_push_split_role_";
constexpr std::string_view kSplitRoleTaskId = "split-role-task";
constexpr std::string_view kSplitRoleConfigId = "split-role-config";
constexpr std::string_view kSplitRoleRaceConfigId = "split-role-race-config";
constexpr std::string_view kProvenanceTaskId = "push-provenance-task";
constexpr std::string_view kLocalProvenanceConfigId = "local-provenance-config";
constexpr std::string_view kExternalProvenanceConfigId = "external-provenance-config";
constexpr std::string_view kTransitionProvenanceConfigId = "transition-provenance-config";
constexpr std::string_view kTaskIdColumn = "id";
constexpr std::string_view kDeleteProvenanceTaskOperation = "delete provenance test task";
constexpr std::string_view kSplitRoleSetupOperation = "set up split-role push store";
constexpr std::string_view kSplitRoleCleanupOperation = "clean up split-role push store";
constexpr std::string_view kSplitRolePassword = "a2a_split_role_password";
constexpr auto kSplitRoleBlockedTimeout = std::chrono::milliseconds(250);

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

void AppendKeywordValue(std::string& dsn, std::string_view keyword, std::string_view value) {
  if (value.empty()) {
    return;
  }
  dsn.append(keyword);
  dsn.append("='");
  for (const char character : value) {
    if (character == '\'' || character == '\\') {
      dsn.push_back('\\');
    }
    dsn.push_back(character);
  }
  dsn.append("' ");
}

[[nodiscard]] std::string BuildEquivalentKeywordDsn(std::string_view dsn, bool include_port = true) {
  char* error = nullptr;
  Conninfo options(PQconninfoParse(std::string(dsn).c_str(), &error));
  if (error != nullptr) {
    PQfreemem(error);
  }
  if (options == nullptr) {
    return {};
  }
  std::string equivalent;
  AppendKeywordValue(equivalent, kConninfoPassword, ConninfoValue(options.get(), kConninfoPassword));
  AppendKeywordValue(equivalent, kConninfoDatabase, ConninfoValue(options.get(), kConninfoDatabase));
  AppendKeywordValue(equivalent, kConninfoHost, ConninfoValue(options.get(), kConninfoHost));
  AppendKeywordValue(equivalent, kConninfoUser, ConninfoValue(options.get(), kConninfoUser));
  if (include_port) {
    AppendKeywordValue(equivalent, kConninfoPort, ConninfoValue(options.get(), kConninfoPort));
  }
  AppendKeywordValue(equivalent, kConninfoApplicationName, kIdentityApplicationName);
  return equivalent;
}

void AppendUriEncoded(std::string& output, std::string_view value) {
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.' || character == '~') {
      output.push_back(static_cast<char>(character));
      continue;
    }
    output.push_back('%');
    output.push_back(kHexDigits[character >> 4U]);
    output.push_back(kHexDigits[character & kLowNibbleMask]);
  }
}

[[nodiscard]] std::string BuildEquivalentUriDsn(std::string_view dsn) {
  char* error = nullptr;
  Conninfo options(PQconninfoParse(std::string(dsn).c_str(), &error));
  if (error != nullptr) {
    PQfreemem(error);
  }
  if (options == nullptr) {
    return {};
  }
  std::string equivalent(kUriScheme);
  AppendUriEncoded(equivalent, ConninfoValue(options.get(), kConninfoUser));
  equivalent.push_back(':');
  AppendUriEncoded(equivalent, ConninfoValue(options.get(), kConninfoPassword));
  equivalent.push_back('@');
  equivalent.append(ConninfoValue(options.get(), kConninfoHost));
  equivalent.push_back(':');
  equivalent.append(ConninfoValue(options.get(), kConninfoPort));
  equivalent.push_back('/');
  AppendUriEncoded(equivalent, ConninfoValue(options.get(), kConninfoDatabase));
  return equivalent;
}

[[nodiscard]] std::string BuildMaintenanceRoleSetupSql(std::string_view role, std::string_view schema,
                                                       std::string_view task_table) {
  const std::string quoted_role = a2a::server::stores::QuoteSqlIdentifier(role);
  std::string sql = "CREATE ROLE ";
  sql.append(quoted_role);
  sql.append(" NOLOGIN; GRANT USAGE ON SCHEMA ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(schema));
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.append("; GRANT DELETE ON ");
  sql.append(task_table);
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildSetRoleSql(std::string_view role) {
  std::string sql = "SET ROLE ";
  sql.append(a2a::server::stores::QuoteSqlIdentifier(role));
  return sql;
}

[[nodiscard]] std::string BuildDropRoleSql(std::string_view role) {
  const std::string quoted_role = a2a::server::stores::QuoteSqlIdentifier(role);
  std::string sql = "DROP OWNED BY ";
  sql.append(quoted_role);
  sql.append("; DROP ROLE ");
  sql.append(quoted_role);
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string MakePostgresTestRole(std::string_view prefix) {
  const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::string(prefix) + std::to_string(ticks);
}

[[nodiscard]] std::string BuildRoleDsn(std::string_view dsn, std::string_view role) {
  char* error = nullptr;
  Conninfo options(PQconninfoParse(std::string(dsn).c_str(), &error));
  if (error != nullptr) {
    PQfreemem(error);
  }
  if (options == nullptr) {
    return {};
  }
  std::string role_dsn;
  AppendKeywordValue(role_dsn, kConninfoPassword, kSplitRolePassword);
  AppendKeywordValue(role_dsn, kConninfoDatabase, ConninfoValue(options.get(), kConninfoDatabase));
  AppendKeywordValue(role_dsn, kConninfoHost, ConninfoValue(options.get(), kConninfoHost));
  AppendKeywordValue(role_dsn, kConninfoUser, role);
  AppendKeywordValue(role_dsn, kConninfoPort, ConninfoValue(options.get(), kConninfoPort));
  AppendKeywordValue(role_dsn, kConninfoApplicationName, kIdentityApplicationName);
  return role_dsn;
}

[[nodiscard]] std::string BuildSplitRoleSetupSql(std::string_view role, std::string_view database,
                                                 std::string_view schema) {
  const std::string quoted_role = a2a::server::stores::QuoteSqlIdentifier(role);
  const std::string quoted_database = a2a::server::stores::QuoteSqlIdentifier(database);
  const std::string quoted_schema = a2a::server::stores::QuoteSqlIdentifier(schema);
  const std::string push_table = a2a::server::stores::PushTable(schema);
  const std::string push_sequence =
      a2a::server::stores::QualifiedSqlIdentifier(schema, a2a::server::stores::kPushCreatedSequenceName);
  std::string sql = "CREATE ROLE ";
  sql.append(quoted_role);
  sql.append(" LOGIN PASSWORD '");
  sql.append(kSplitRolePassword);
  sql.append("'; GRANT CONNECT ON DATABASE ");
  sql.append(quoted_database);
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.append("; GRANT USAGE ON SCHEMA ");
  sql.append(quoted_schema);
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.append("; GRANT SELECT, INSERT, UPDATE, DELETE ON ");
  sql.append(push_table);
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.append("; GRANT USAGE, SELECT ON SEQUENCE ");
  sql.append(push_sequence);
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.push_back(';');
  return sql;
}

class ScopedPostgresRole final {
 public:
  ScopedPostgresRole(PGconn* connection, std::string role, std::string database, std::string schema)
      : connection_(connection), role_(std::move(role)), database_(std::move(database)), schema_(std::move(schema)) {}

  ScopedPostgresRole(const ScopedPostgresRole&) = delete;
  ScopedPostgresRole& operator=(const ScopedPostgresRole&) = delete;

  [[nodiscard]] a2a::core::Result<void> Create() {
    auto created = a2a::server::stores::Exec(connection_, BuildSplitRoleSetupSql(role_, database_, schema_),
                                             kSplitRoleSetupOperation);
    created_ = created.ok();
    return created;
  }

  ~ScopedPostgresRole() {
    if (created_) {
      (void)a2a::server::stores::Exec(connection_, BuildDropRoleSql(role_), kSplitRoleCleanupOperation);
    }
  }

  [[nodiscard]] const std::string& role() const noexcept { return role_; }

 private:
  PGconn* connection_;
  std::string role_;
  std::string database_;
  std::string schema_;
  bool created_ = false;
};

[[nodiscard]] std::string BuildDeleteByTaskIdSql(std::string_view table, std::string_view id_column,
                                                 std::string_view task_id);

[[nodiscard]] a2a::core::Result<void> DeletePostgresTask(a2a::server::stores::PostgresPushNotificationStore& push_store,
                                                         const a2a::server::stores::PostgresStoreOptions& options,
                                                         std::string_view task_id) {
  auto connection = push_store.AcquireConnectionForTesting();
  if (!connection.ok()) {
    return connection.error();
  }
  return a2a::server::stores::Exec(
      connection.value().get(),
      BuildDeleteByTaskIdSql(a2a::server::stores::TaskTable(options.schema), kTaskIdColumn, task_id),
      kDeleteProvenanceTaskOperation);
}

void ExpectPushConfigPresent(a2a::server::stores::PostgresPushNotificationStore& store, std::string_view task_id,
                             std::string_view config_id) {
  const auto config = store.Get(task_id, config_id);
  ASSERT_TRUE(config.ok());
  EXPECT_EQ(config.value().id(), config_id);
}

void ExpectPushConfigMissing(a2a::server::stores::PostgresPushNotificationStore& store, std::string_view task_id,
                             std::string_view config_id) {
  EXPECT_FALSE(store.Get(task_id, config_id).ok());
}

struct SplitRoleCreateOutcome final {
  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> result;
  a2a::server::stores::PostgresOperationDiagnostics diagnostics;
};

[[nodiscard]] std::string BuildDeleteByTaskIdSql(std::string_view table, std::string_view id_column,
                                                 std::string_view task_id) {
  std::string sql = "DELETE FROM ";
  sql.append(table);
  sql.append(" WHERE ");
  sql.append(id_column);
  sql.append(" = '");
  sql.append(task_id);
  sql.push_back('\'');
  return sql;
}

[[nodiscard]] std::string BuildDeleteAllSql(std::string_view table) {
  // An unfiltered DELETE exercises task-table DELETE privilege without also
  // requiring SELECT privilege for a predicate column.
  std::string sql = "DELETE FROM ";
  sql.append(table);
  return sql;
}

void ExpectEquivalentStorageIdentities(const a2a::server::stores::PostgresStorageIdentity& expected,
                                       const a2a::server::stores::PostgresStorageIdentity& identical,
                                       const a2a::server::stores::PostgresStorageIdentity& reordered,
                                       const a2a::server::stores::PostgresStorageIdentity& uri) {
  EXPECT_EQ(expected, identical);
  EXPECT_EQ(expected, reordered);
  EXPECT_EQ(expected, uri);
}

struct ConcurrentDeleteOutcome final {
  std::future_status create_wait_status;
  a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig> create_result;
  bool config_remains;
};

[[nodiscard]] a2a::core::Result<ConcurrentDeleteOutcome> RunConcurrentPushConfigDelete(
    a2a::server::stores::PostgresPushNotificationStore& push_store,
    const a2a::server::stores::PostgresStoreOptions& options) {
  auto deletion_connection = push_store.AcquireConnectionForTesting();
  if (!deletion_connection.ok()) {
    return deletion_connection.error();
  }
  a2a::server::stores::Transaction deletion_transaction(deletion_connection.value().get());
  const auto begun = deletion_transaction.Begin();
  if (!begun.ok()) {
    return begun.error();
  }
  std::string delete_sql = "DELETE FROM ";
  delete_sql.append(a2a::server::stores::TaskTable(options.schema));
  delete_sql.append(" WHERE id = '");
  delete_sql.append(kAtomicCreateTaskId);
  delete_sql.push_back('\'');
  const auto deleted =
      a2a::server::stores::Exec(deletion_connection.value().get(), delete_sql, kHoldConcurrentDeleteOperation);
  if (!deleted.ok()) {
    return deleted.error();
  }

  std::barrier create_started(2);
  auto create = std::async(std::launch::async, [&] {
    create_started.arrive_and_wait();
    return push_store.CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId),
                                                                               std::string(kAtomicCreateConfigId)));
  });
  create_started.arrive_and_wait();
  const std::future_status wait_status = create.wait_for(kConcurrentCreateBlockedTimeout);
  const auto committed = deletion_transaction.Commit();
  if (!committed.ok()) {
    return committed.error();
  }
  auto create_result = create.get();
  const bool config_remains = push_store.Get(kAtomicCreateTaskId, kAtomicCreateConfigId).ok();
  return ConcurrentDeleteOutcome{
      .create_wait_status = wait_status, .create_result = std::move(create_result), .config_remains = config_remains};
}

void ExpectConcurrentDeleteOutcome(const a2a::core::Result<ConcurrentDeleteOutcome>& outcome) {
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome.value().create_wait_status, std::future_status::timeout);
  ASSERT_FALSE(outcome.value().create_result.ok());
  EXPECT_EQ(outcome.value().create_result.error().protocol_code().value_or(std::string{}),
            a2a::core::protocol_codes::kTaskNotFound);
  EXPECT_FALSE(outcome.value().config_remains);
}

struct LeastPrivilegeDeleteOutcome final {
  bool config_removed;
  bool direct_push_delete_denied;
};

[[nodiscard]] a2a::core::Result<LeastPrivilegeDeleteOutcome> RunLeastPrivilegeTaskDelete(
    a2a::server::stores::PostgresPushNotificationStore& push_store,
    const a2a::server::stores::PostgresStoreOptions& options) {
  auto connection = push_store.AcquireConnectionForTesting();
  if (!connection.ok()) {
    return connection.error();
  }
  std::string role(kMaintenanceRolePrefix);
  role.append(std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const std::string task_table = a2a::server::stores::TaskTable(options.schema);
  const std::string push_table = a2a::server::stores::PushTable(options.schema);
  const auto setup = a2a::server::stores::Exec(
      connection.value().get(), BuildMaintenanceRoleSetupSql(role, options.schema, task_table), kRoleSetupOperation);
  if (!setup.ok()) {
    return setup.error();
  }
  const auto switched =
      a2a::server::stores::Exec(connection.value().get(), BuildSetRoleSql(role), kRoleSwitchOperation);
  if (!switched.ok()) {
    return switched.error();
  }
  const auto deleted =
      a2a::server::stores::Exec(connection.value().get(), BuildDeleteAllSql(task_table), kRoleTaskDeleteOperation);
  if (!deleted.ok()) {
    return deleted.error();
  }
  const auto reset =
      a2a::server::stores::Exec(connection.value().get(), std::string(kResetRoleSql), kRoleResetOperation);
  if (!reset.ok()) {
    return reset.error();
  }
  const bool config_removed = !push_store.Get(kAtomicCreateTaskId, kAtomicCreateConfigId).ok();
  const auto reswitched =
      a2a::server::stores::Exec(connection.value().get(), BuildSetRoleSql(role), kRoleSwitchOperation);
  if (!reswitched.ok()) {
    return reswitched.error();
  }
  const auto direct_delete = a2a::server::stores::Exec(
      connection.value().get(), BuildDeleteByTaskIdSql(push_table, kPushTaskIdColumn, kAtomicCreateTaskId),
      kRolePushDeleteOperation);
  const auto final_reset =
      a2a::server::stores::Exec(connection.value().get(), std::string(kResetRoleSql), kRoleResetOperation);
  if (!final_reset.ok()) {
    return final_reset.error();
  }
  const auto cleanup =
      a2a::server::stores::Exec(connection.value().get(), BuildDropRoleSql(role), kRoleCleanupOperation);
  if (!cleanup.ok()) {
    return cleanup.error();
  }
  return LeastPrivilegeDeleteOutcome{.config_removed = config_removed,
                                     .direct_push_delete_denied = !direct_delete.ok()};
}

void ExpectLeastPrivilegeDeleteOutcome(const a2a::core::Result<LeastPrivilegeDeleteOutcome>& outcome) {
  ASSERT_TRUE(outcome.ok());
  EXPECT_TRUE(outcome.value().config_removed);
  EXPECT_TRUE(outcome.value().direct_push_delete_denied);
}

void ExpectSingleTaskAwarePushConfigUpsert() {
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      1U);
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            0U);
}

void ExpectStoredLargePushConfig(const a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>& stored) {
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored.value().url(), kLargeConfigUpdatedUrl);
  EXPECT_EQ(stored.value().token().size(), kLargeConfigMetadataSize);
  EXPECT_EQ(stored.value().authentication().credentials().size(), kLargeConfigMetadataSize);
}

template <typename Insert>
[[nodiscard]] bool RunDeterministicallyInterleavedInserts(Insert insert) {
  std::barrier synchronization_point(2);
  std::atomic<bool> succeeded{true};
  const auto insert_and_record = [&](std::size_t connection, std::string_view id) {
    if (!insert(connection, id)) {
      succeeded.store(false, std::memory_order_relaxed);
    }
  };
  std::thread connection_a([&] {
    insert_and_record(0, kConcurrentIds[0]);
    synchronization_point.arrive_and_wait();
    synchronization_point.arrive_and_wait();
    insert_and_record(0, kConcurrentIds[2]);
    synchronization_point.arrive_and_wait();
  });
  std::thread connection_b([&] {
    synchronization_point.arrive_and_wait();
    insert_and_record(1, kConcurrentIds[1]);
    synchronization_point.arrive_and_wait();
    synchronization_point.arrive_and_wait();
    insert_and_record(1, kConcurrentIds[3]);
  });
  connection_a.join();
  connection_b.join();
  return succeeded.load(std::memory_order_relaxed);
}

void ExpectTaskListOrder(a2a::server::stores::PostgresTaskStore& store) {
  a2a::server::ListTasksRequest request;
  const auto all_tasks = store.List(request);
  ASSERT_TRUE(all_tasks.ok());
  ASSERT_EQ(all_tasks.value().tasks.size(), kConcurrentIds.size());
  for (std::size_t index = 0; index < kConcurrentIds.size(); ++index) {
    EXPECT_EQ(all_tasks.value().tasks[index].id(), kConcurrentIds[index]);
  }
}

void ExpectTaskPaginationOrder(a2a::server::stores::PostgresTaskStore& store) {
  a2a::server::ListTasksRequest request;
  request.page_size = kConcurrentPageSize;
  for (const std::string_view expected_id : kConcurrentIds) {
    const auto page = store.List(request);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().tasks.size(), kConcurrentPageSize);
    EXPECT_EQ(page.value().tasks.front().id(), expected_id);
    request.page_token = page.value().next_page_token;
  }
  EXPECT_TRUE(request.page_token.empty());
}

void ExpectPushConfigListOrder(a2a::server::stores::PostgresPushNotificationStore& store, std::string_view task_id) {
  const auto all_configs = store.List(task_id);
  ASSERT_TRUE(all_configs.ok());
  ASSERT_EQ(all_configs.value().configs_size(), static_cast<int>(kConcurrentIds.size()));
  for (std::size_t index = 0; index < kConcurrentIds.size(); ++index) {
    EXPECT_EQ(all_configs.value().configs(static_cast<int>(index)).id(), kConcurrentIds[index]);
  }
}

void ExpectPushConfigPaginationOrder(a2a::server::stores::PostgresPushNotificationStore& store,
                                     std::string_view task_id) {
  std::string page_token;
  for (const std::string_view expected_id : kConcurrentIds) {
    const auto page = store.List(task_id, static_cast<int>(kConcurrentPageSize), page_token);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.value().configs_size(), static_cast<int>(kConcurrentPageSize));
    EXPECT_EQ(page.value().configs(0).id(), expected_id);
    page_token = page.value().next_page_token();
  }
  EXPECT_TRUE(page_token.empty());
}

void AddPostgresTask(a2a::server::stores::PostgresTaskStore& store, std::string_view task_id,
                     std::string_view context_id, lf::a2a::v1::TaskState state, int timestamp_seconds) {
  ASSERT_TRUE(store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(std::string(task_id), std::string(context_id),
                                                                          state, timestamp_seconds))
                  .ok());
}

void SeedPostgresFilteredPaginationTasks(a2a::server::stores::PostgresTaskStore& store) {
  AddPostgresTask(store, kOldTargetTaskId, kTargetContext, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  AddPostgresTask(store, kNewTargetTaskId, kTargetContext, lf::a2a::v1::TASK_STATE_WORKING,
                  kNewTargetTaskTimestampSeconds);
  AddPostgresTask(store, kOtherContextTaskId, kOtherContext, lf::a2a::v1::TASK_STATE_WORKING,
                  kOtherContextTaskTimestampSeconds);
  AddPostgresTask(store, kCompletedTargetTaskId, kTargetContext, lf::a2a::v1::TASK_STATE_COMPLETED,
                  kCompletedTargetTaskTimestampSeconds);
}

void SeedPushConfigTasks(a2a::server::stores::PostgresTaskStore& store) {
  AddPostgresTask(store, a2a::tests::store_conformance::kPushTask, "push-context", lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  AddPostgresTask(store, a2a::tests::store_conformance::kOrderedPushTask, "push-context",
                  lf::a2a::v1::TASK_STATE_WORKING, kOldTargetTaskTimestampSeconds);
  AddPostgresTask(store, "shared-postgres-task", "push-context", lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
}

void ExpectSinglePushConfigListCommand() {
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            0U);
  EXPECT_EQ(
      diagnostics
          .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigListCount)],
      0U);
  EXPECT_EQ(
      diagnostics
          .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigListSelect)],
      1U);
}

void ExpectNoPushConfigListCommand() {
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            0U);
  EXPECT_EQ(
      diagnostics
          .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigListCount)],
      0U);
  EXPECT_EQ(
      diagnostics
          .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigListSelect)],
      0U);
}

void ResetPushConfigListDiagnostics() { a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting(); }

void SeedPushConfigListEdgeCases(a2a::server::stores::PostgresTaskStore& task_store,
                                 a2a::server::stores::PostgresPushNotificationStore& push_store) {
  AddPostgresTask(task_store, kEmptyListTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  AddPostgresTask(task_store, kListEdgeTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kListEdgeTaskId),
                                                                            std::string(kFirstListConfigId)))
                  .ok());
  ASSERT_TRUE(push_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kListEdgeTaskId),
                                                                            std::string(kSecondListConfigId)))
                  .ok());
}

void ExpectEmptyPushConfigList(a2a::server::stores::PostgresPushNotificationStore& store) {
  ResetPushConfigListDiagnostics();
  const auto result = store.List(kEmptyListTaskId);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value().configs().empty());
  ExpectSinglePushConfigListCommand();
}

void ExpectUnboundedPushConfigList(a2a::server::stores::PostgresPushNotificationStore& store) {
  ResetPushConfigListDiagnostics();
  const auto result = store.List(kListEdgeTaskId);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.value().configs_size(), kPushListConfigCount);
  EXPECT_EQ(result.value().configs(0).id(), kFirstListConfigId);
  EXPECT_EQ(result.value().configs(1).id(), kSecondListConfigId);
  EXPECT_TRUE(result.value().next_page_token().empty());
  ExpectSinglePushConfigListCommand();
}

void ExpectBoundedPushConfigList(a2a::server::stores::PostgresPushNotificationStore& store) {
  ResetPushConfigListDiagnostics();
  const auto result = store.List(kListEdgeTaskId, kBoundedPushListPageSize);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.value().configs_size(), kBoundedPushListPageSize);
  EXPECT_EQ(result.value().configs(0).id(), kFirstListConfigId);
  EXPECT_EQ(result.value().next_page_token(), kFirstPageToken);
  ExpectSinglePushConfigListCommand();
}

void ExpectPushConfigEndPage(a2a::server::stores::PostgresPushNotificationStore& store) {
  ResetPushConfigListDiagnostics();
  const auto result = store.List(kListEdgeTaskId, kBoundedPushListPageSize, kTotalConfigCountToken);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value().configs().empty());
  EXPECT_TRUE(result.value().next_page_token().empty());
  ExpectSinglePushConfigListCommand();
}

void ExpectPushConfigOutOfRangePage(a2a::server::stores::PostgresPushNotificationStore& store) {
  ResetPushConfigListDiagnostics();
  const auto result = store.List(kListEdgeTaskId, kBoundedPushListPageSize, kOutOfRangeConfigToken);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  ExpectSinglePushConfigListCommand();
}

void ExpectPushConfigTokenBeyondPostgresBigint(a2a::server::stores::PostgresPushNotificationStore& store) {
  ResetPushConfigListDiagnostics();
  const auto result = store.List(kListEdgeTaskId, kBoundedPushListPageSize, kBeyondPostgresBigintToken);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_EQ(result.error().message(), a2a::server::stores::kPageTokenOutOfRangeMessage);
  ExpectNoPushConfigListCommand();
}

void ExpectMissingPushConfigTask(a2a::server::stores::PostgresPushNotificationStore& store) {
  ResetPushConfigListDiagnostics();
  const auto result = store.List(kMissingListTaskId);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
  ExpectSinglePushConfigListCommand();
}

void ExpectFilteredPostgresListPage(const a2a::server::ListTasksResponse& page, std::string_view expected_task_id,
                                    bool expect_next_page) {
  EXPECT_EQ(page.total_size, kFilteredTaskCount);
  ASSERT_EQ(page.tasks.size(), kSingleTaskPageSize);
  EXPECT_EQ(page.tasks.front().id(), expected_task_id);
  EXPECT_EQ(!page.next_page_token.empty(), expect_next_page);
}

TEST(StoreConformanceTest, PostgresStoreBundleRejectsInvalidSchemaBeforeConnecting) {
  constexpr std::string_view kInvalidConnectionString = "postgresql://invalid-host.invalid/a2a";
  constexpr std::array<std::string_view, 5> kInvalidSchemas = {"", "1invalid", "bad-name", "bad.schema", "bad;schema"};

  for (const std::string_view schema : kInvalidSchemas) {
    const a2a::server::stores::PostgresStoreFactory factory(
        {.connection_string = std::string(kInvalidConnectionString), .schema = std::string(schema)});

    const auto result = factory.CreateStoreBundle();

    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  }
}

TEST(StoreConformanceTest, PostgresStoreConstructorsRejectInvalidSchemaBeforeConnecting) {
  constexpr std::string_view kInvalidConnectionString = "postgresql://invalid-host.invalid/a2a";
  constexpr std::string_view kInvalidSchema = "bad-schema";

  EXPECT_THROW((void)a2a::server::stores::PostgresTaskStore(
                   {.connection_string = std::string(kInvalidConnectionString), .schema = std::string(kInvalidSchema)}),
               std::invalid_argument);
  EXPECT_THROW((void)a2a::server::stores::PostgresPushNotificationStore(
                   {.connection_string = std::string(kInvalidConnectionString), .schema = std::string(kInvalidSchema)}),
               std::invalid_argument);
}

TEST(StoreConformanceTest, PostgresConnectionPoolRejectsZeroSizeBeforeConnecting) {
  constexpr std::string_view kInvalidConnectionString = "postgresql://invalid-host.invalid/a2a";

  EXPECT_THROW((void)a2a::server::stores::PostgresConnectionPool(std::string(kInvalidConnectionString), 0),
               std::invalid_argument);
}

TEST(StoreConformanceTest, PostgresOptionsDefaultToFourConnections) {
  const a2a::server::stores::PostgresStoreOptions options;

  EXPECT_EQ(options.connection_pool_size, a2a::server::stores::kDefaultPostgresConnectionPoolSize);
  EXPECT_EQ(options.connection_pool_size, 4U);
}

TEST(StoreConformanceTest, PostgresStorageIdentityNormalizesEffectiveConnectionAttributes) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string keyword_dsn = BuildEquivalentKeywordDsn(dsn_value);
  const std::string uri_dsn = BuildEquivalentUriDsn(dsn_value);
  ASSERT_FALSE(keyword_dsn.empty());
  ASSERT_FALSE(uri_dsn.empty());
  a2a::server::stores::PostgresConnectionPool original(dsn_value, 1U);
  a2a::server::stores::PostgresConnectionPool reordered(keyword_dsn, 1U);
  a2a::server::stores::PostgresConnectionPool uri(uri_dsn, 1U);
  const auto identity = original.StorageIdentity(std::string(kIdentitySchema));

  ExpectEquivalentStorageIdentities(identity, original.StorageIdentity(std::string(kIdentitySchema)),
                                    reordered.StorageIdentity(std::string(kIdentitySchema)),
                                    uri.StorageIdentity(std::string(kIdentitySchema)));
}

TEST(StoreConformanceTest, PostgresStorageIdentityNormalizesOmittedDefaultPort) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  a2a::server::stores::PostgresConnectionPool explicit_port(dsn_value, 1U);
  if (explicit_port.StorageIdentity(std::string(kDefaultPortIdentitySchema)).port != kDefaultPostgresPort) {
    GTEST_SKIP() << kNonDefaultPortSkipMessage;
  }
  const char* environment_port = std::getenv(kPgPortEnvironmentVariable);
  if (environment_port != nullptr && std::string_view(environment_port) != kDefaultPostgresPort) {
    GTEST_SKIP() << kNonDefaultPgPortSkipMessage;
  }
  a2a::server::stores::PostgresConnectionPool default_port(BuildEquivalentKeywordDsn(dsn_value, false), 1U);
  EXPECT_EQ(explicit_port.StorageIdentity(std::string(kDefaultPortIdentitySchema)),
            default_port.StorageIdentity(std::string(kDefaultPortIdentitySchema)));
}

TEST(StoreConformanceTest, PostgresStorageIdentityDistinguishesStorageCoordinates) {
  a2a::server::stores::PostgresStorageIdentity identity{
      .host = "database.example", .port = "5432", .database = "a2a", .schema = "public"};
  auto different = identity;
  different.database = "other";
  EXPECT_NE(identity, different);
  different = identity;
  different.host = "replica.example";
  EXPECT_NE(identity, different);
  different = identity;
  different.port = "5433";
  EXPECT_NE(identity, different);
  different = identity;
  different.schema = "tenant";
  EXPECT_NE(identity, different);
}

TEST(StoreConformanceTest, PostgresConnectionErrorsDoNotExposeSecrets) {
  try {
    (void)a2a::server::stores::PostgresConnectionPool(std::string(kSecretDsn), 1U);
    FAIL() << kUnexpectedSecretDsnConnection;
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string_view(error.what()).find(kSecretDsnPassword), std::string_view::npos);
  }
}

TEST(StoreConformanceTest, PostgresFactoryRejectsZeroPoolSizeBeforeConnecting) {
  constexpr std::string_view kInvalidConnectionString = "postgresql://invalid-host.invalid/a2a";
  const a2a::server::stores::PostgresStoreFactory factory(
      {.connection_string = std::string(kInvalidConnectionString), .connection_pool_size = 0U});

  const auto result = factory.CreateStoreBundle();

  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_EQ(result.error().message(), a2a::server::stores::kPostgresConnectionPoolSizeValidationMessage);
}

TEST(StoreConformanceTest, PostgresBundleSharesConfiguredConnectionPool) {
  constexpr std::size_t kCustomPoolSize = 2U;
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreFactory factory({.connection_string = dsn_value,
                                                           .schema = MakePostgresTestSchema("shared_pool"),
                                                           .connection_pool_size = kCustomPoolSize});

  auto bundle = factory.CreateStoreBundle();

  ASSERT_TRUE(bundle.ok());
  const auto* task_store = dynamic_cast<a2a::server::stores::PostgresTaskStore*>(bundle.value().task_store.get());
  const auto* push_store =
      dynamic_cast<a2a::server::stores::PostgresPushNotificationStore*>(bundle.value().push_store.get());
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(push_store, nullptr);
  ASSERT_EQ(task_store->connection_pool_for_testing(), push_store->connection_pool_for_testing());
  EXPECT_EQ(task_store->connection_pool_for_testing()->capacity(), kCustomPoolSize);
}

TEST(StoreConformanceTest, PostgresTaskStore) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("task");
  a2a::tests::store_conformance::RunTaskStoreConformance([&] {
    return std::make_unique<a2a::server::stores::PostgresTaskStore>(
        a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});
  });

  a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn, .schema = schema};
  a2a::server::stores::PostgresTaskStore first(options);
  a2a::server::stores::PostgresTaskStore second(options);
  ASSERT_TRUE(first
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask("shared-postgres-task", "shared-context",
                                                                          lf::a2a::v1::TASK_STATE_WORKING, 2000))
                  .ok());
  const auto shared = second.Get("shared-postgres-task");
  ASSERT_TRUE(shared.ok());
  EXPECT_EQ(shared.value().id(), "shared-postgres-task");
}

TEST(StoreConformanceTest, PostgresTaskStoreListAppliesFiltersBeforePagination) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("task_filtered_page");
  a2a::server::stores::PostgresTaskStore store(
      a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});
  SeedPostgresFilteredPaginationTasks(store);

  a2a::server::ListTasksRequest request;
  request.context_id = kTargetContext;
  request.status_filter = lf::a2a::v1::TASK_STATE_WORKING;
  request.page_size = kSingleTaskPageSize;

  const auto first_page = store.List(request);
  ASSERT_TRUE(first_page.ok());
  ExpectFilteredPostgresListPage(first_page.value(), kOldTargetTaskId, true);

  request.page_token = first_page.value().next_page_token;
  const auto second_page = store.List(request);
  ASSERT_TRUE(second_page.ok());
  ExpectFilteredPostgresListPage(second_page.value(), kNewTargetTaskId, false);
}

TEST(StoreConformanceTest, PostgresTaskPaginationPreservesCreationOrderAcrossConnections) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("task_concurrent_order");
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn, .schema = schema};
  std::array<a2a::server::stores::PostgresTaskStore, 2> stores = {a2a::server::stores::PostgresTaskStore(options),
                                                                  a2a::server::stores::PostgresTaskStore(options)};
  ASSERT_TRUE(RunDeterministicallyInterleavedInserts([&](std::size_t connection, std::string_view id) {
    return stores[connection]
        .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
            std::string(id), "concurrent-context", lf::a2a::v1::TASK_STATE_WORKING, kOldTargetTaskTimestampSeconds))
        .ok();
  }));
  ExpectTaskListOrder(stores.front());
  ExpectTaskPaginationOrder(stores.front());
}

TEST(StoreConformanceTest, PostgresPushConfigPaginationPreservesCreationOrderAcrossConnections) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("push_concurrent_order");
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn, .schema = schema};
  a2a::server::stores::PostgresTaskStore task_store(options);
  ASSERT_TRUE(task_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask("concurrent-push-task", "concurrent-context",
                                                                          lf::a2a::v1::TASK_STATE_WORKING,
                                                                          kOldTargetTaskTimestampSeconds))
                  .ok());
  std::array<a2a::server::stores::PostgresPushNotificationStore, 2> stores = {
      a2a::server::stores::PostgresPushNotificationStore(options),
      a2a::server::stores::PostgresPushNotificationStore(options)};
  constexpr std::string_view kTaskId = "concurrent-push-task";
  ASSERT_TRUE(RunDeterministicallyInterleavedInserts([&](std::size_t connection, std::string_view id) {
    return stores[connection]
        .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kTaskId), std::string(id)))
        .ok();
  }));
  ExpectPushConfigListOrder(stores.front(), kTaskId);
  ExpectPushConfigPaginationOrder(stores.front(), kTaskId);
}

TEST(StoreConformanceTest, PostgresTaskStorePropagatesAcquireFailures) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("task_acquire_failure");
  a2a::server::stores::PostgresTaskStore store(
      a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto create = store.CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
      "lease-error-task", "lease-error-context", lf::a2a::v1::TASK_STATE_WORKING, 1000));
  ASSERT_FALSE(create.ok());
  ExpectPostgresAcquireFailure(create.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto get = store.Get("lease-error-task");
  ASSERT_FALSE(get.ok());
  ExpectPostgresAcquireFailure(get.error());

  a2a::server::ListTasksRequest list_request;
  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto list = store.List(list_request);
  ASSERT_FALSE(list.ok());
  ExpectPostgresAcquireFailure(list.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto cancel = store.Cancel("lease-error-task");
  ASSERT_FALSE(cancel.ok());
  ExpectPostgresAcquireFailure(cancel.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto append = store.AppendTaskHistory(
      "lease-error-task", a2a::tests::store_conformance::MakeMessage("lease-error-message", "message"),
      a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup);
  ASSERT_FALSE(append.ok());
  ExpectPostgresAcquireFailure(append.error());
}

TEST(StoreConformanceTest, PostgresAppendHistoryReportsLockingReadDiagnostic) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema("history_lock_read_diagnostic");
  a2a::server::stores::PostgresTaskStore store(
      a2a::server::stores::PostgresStoreOptions{.connection_string = dsn_value, .schema = schema});
  AddPostgresTask(store, "history-diagnostic-task", "history-diagnostic-context", lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();

  const auto append = store.AppendTaskHistory(
      "history-diagnostic-task", a2a::tests::store_conformance::MakeMessage("history-diagnostic-message", "entry"),
      a2a::server::TaskStore::HistoryAppendPolicy::kNoDedup);

  ASSERT_TRUE(append.ok());
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(
      diagnostics
          .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskHistoryLockRead)],
      1U);
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            0U);
}

TEST(StoreConformanceTest, PostgresPushNotificationStorePropagatesAcquireFailures) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("push_acquire_failure");
  a2a::server::stores::PostgresPushNotificationStore store(
      a2a::server::stores::PostgresStoreOptions{.connection_string = dsn, .schema = schema});

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto create =
      store.CreateOrUpdate(a2a::tests::store_conformance::MakeConfig("lease-error-task", "lease-error-config"));
  ASSERT_FALSE(create.ok());
  ExpectPostgresAcquireFailure(create.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto get = store.Get("lease-error-task", "lease-error-config");
  ASSERT_FALSE(get.ok());
  ExpectPostgresAcquireFailure(get.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto list = store.List("lease-error-task");
  ASSERT_FALSE(list.ok());
  ExpectPostgresAcquireFailure(list.error());

  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto deleted = store.Delete("lease-error-task", "lease-error-config");
  ASSERT_FALSE(deleted.ok());
  ExpectPostgresAcquireFailure(deleted.error());
}

TEST(StoreConformanceTest, PostgresStoreFactoryPropagatesAcquireFailures) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;

  a2a::server::stores::PostgresStoreFactory task_factory(
      {.connection_string = dsn, .schema = MakePostgresTestSchema("factory_task_acquire_failure")});
  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto task_store = task_factory.CreateTaskStore();
  ASSERT_FALSE(task_store.ok());
  ExpectPostgresAcquireFailure(task_store.error());

  a2a::server::stores::PostgresStoreFactory push_factory(
      {.connection_string = dsn, .schema = MakePostgresTestSchema("factory_push_acquire_failure")});
  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto push_store = push_factory.CreatePushNotificationStore();
  ASSERT_FALSE(push_store.ok());
  ExpectPostgresAcquireFailure(push_store.error());

  a2a::server::stores::PostgresStoreFactory bundle_factory(
      {.connection_string = dsn, .schema = MakePostgresTestSchema("factory_bundle_acquire_failure")});
  a2a::server::stores::FailNextPostgresAcquireForTesting(MakePostgresAcquireFailureForTesting());
  const auto bundle = bundle_factory.CreateStoreBundle();
  ASSERT_FALSE(bundle.ok());
  ExpectPostgresAcquireFailure(bundle.error());
}

TEST(StoreConformanceTest, PostgresPushNotificationStore) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string dsn = dsn_value;
  const std::string schema = MakePostgresTestSchema("push");
  a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn, .schema = schema};
  a2a::server::stores::PostgresTaskStore task_store(options);
  SeedPushConfigTasks(task_store);
  a2a::tests::store_conformance::RunPushNotificationStoreConformance(
      [&] { return std::make_unique<a2a::server::stores::PostgresPushNotificationStore>(options); },
      a2a::tests::store_conformance::MissingTaskListBehavior::kReturnsTaskNotFound);

  a2a::server::stores::PostgresPushNotificationStore first(options);
  a2a::server::stores::PostgresPushNotificationStore second(options);
  ASSERT_TRUE(
      first.CreateOrUpdate(a2a::tests::store_conformance::MakeConfig("shared-postgres-task", "shared-postgres-config"))
          .ok());
  const auto shared = second.Get("shared-postgres-task", "shared-postgres-config");
  ASSERT_TRUE(shared.ok());
  EXPECT_EQ(shared.value().id(), "shared-postgres-config");

  ResetPushConfigListDiagnostics();
  ASSERT_TRUE(first.List("shared-postgres-task").ok());
  ExpectSinglePushConfigListCommand();
}

TEST(StoreConformanceTest, PostgresPushConfigListEdgeCasesUseOneCommand) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn_value,
                                                          .schema = MakePostgresTestSchema("push_list_edges")};
  a2a::server::stores::PostgresTaskStore task_store(options);
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  SeedPushConfigListEdgeCases(task_store, push_store);
  ExpectEmptyPushConfigList(push_store);
  ExpectUnboundedPushConfigList(push_store);
  ExpectBoundedPushConfigList(push_store);
  ExpectPushConfigEndPage(push_store);
  ExpectPushConfigOutOfRangePage(push_store);
  ExpectPushConfigTokenBeyondPostgresBigint(push_store);
  ExpectMissingPushConfigTask(push_store);
}

TEST(StoreConformanceTest, PostgresPushConfigCreateIsAtomicAndTaskAware) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn_value,
                                                          .schema = MakePostgresTestSchema("push_atomic_create")};
  a2a::server::stores::PostgresTaskStore task_store(options);
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);

  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto created = push_store.CreateOrUpdate(
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId)));
  ASSERT_TRUE(created.ok());
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      1U);
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            0U);

  const auto missing = push_store.CreateOrUpdate(
      a2a::tests::store_conformance::MakeConfig("missing-atomic-create-task", std::string(kAtomicCreateConfigId)));
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
}

TEST(StoreConformanceTest, PostgresPushConfigUsesSuppliedAuthoritativeTaskStore) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn_value,
                                                          .schema = MakePostgresTestSchema("push_mixed_task_store")};
  a2a::server::InMemoryTaskStore task_store;
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  ASSERT_TRUE(task_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
                      std::string(kMixedCreateTaskId), std::string(kPushListContextId), lf::a2a::v1::TASK_STATE_WORKING,
                      kOldTargetTaskTimestampSeconds))
                  .ok());

  const auto created = push_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kMixedCreateTaskId), std::string(kMixedCreateConfigId)),
      task_store);

  ASSERT_TRUE(created.ok());
  const auto fetched = push_store.Get(kMixedCreateTaskId, kMixedCreateConfigId);
  ASSERT_TRUE(fetched.ok());
  EXPECT_EQ(fetched.value().id(), kMixedCreateConfigId);
}

TEST(StoreConformanceTest, EquivalentPostgresDsnsUseAtomicSentinelUpsertForLargeConfig) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema("push_equivalent_dsn");
  const a2a::server::stores::PostgresStoreOptions task_options{.connection_string = dsn_value, .schema = schema};
  const a2a::server::stores::PostgresStoreOptions push_options{
      .connection_string = BuildEquivalentKeywordDsn(dsn_value), .schema = schema};
  a2a::server::stores::PostgresTaskStore task_store(task_options);
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  auto config =
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId));
  config.set_token(std::string(kLargeConfigMetadataSize, kLargeConfigMetadataFill));
  config.mutable_authentication()->set_scheme(std::string(kLargeConfigAuthScheme));
  config.mutable_authentication()->set_credentials(std::string(kLargeConfigMetadataSize, kLargeConfigMetadataFill));

  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, task_store).ok());
  ExpectSingleTaskAwarePushConfigUpsert();
  config.set_url(std::string(kLargeConfigUpdatedUrl));
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, task_store).ok());
  ExpectSingleTaskAwarePushConfigUpsert();

  const auto stored = push_store.Get(kAtomicCreateTaskId, kAtomicCreateConfigId);
  ExpectStoredLargePushConfig(stored);
}

TEST(StoreConformanceTest, PostgresPushConfigCreateReturnsTaskNotFoundWhenConcurrentDeletionWins) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn_value,
                                                          .schema = MakePostgresTestSchema("push_concurrent_delete")};
  a2a::server::stores::PostgresTaskStore task_store(options);
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId),
                                                                            std::string(kAtomicCreateConfigId)))
                  .ok());

  const auto outcome = RunConcurrentPushConfigDelete(push_store, options);
  ExpectConcurrentDeleteOutcome(outcome);
}

TEST(StoreConformanceTest, DifferentPostgresRolesUseSafeSplitRoleCreatePath) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema("push_split_role");
  const a2a::server::stores::PostgresStoreOptions owner_options{
      .connection_string = dsn_value, .schema = schema, .connection_pool_size = 2U};
  a2a::server::stores::PostgresTaskStore task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_push_store(owner_options);
  AddPostgresTask(task_store, kSplitRoleTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);

  auto admin_connection = owner_push_store.AcquireConnectionForTesting();
  ASSERT_TRUE(admin_connection.ok());
  ScopedPostgresRole role(admin_connection.value().get(), MakePostgresTestRole(kSplitRolePrefix),
                          task_store.execution_identity().storage.database, schema);
  ASSERT_TRUE(role.Create().ok());
  const std::string role_dsn = BuildRoleDsn(dsn_value, role.role());
  ASSERT_FALSE(role_dsn.empty());

  {
    const a2a::server::stores::PostgresStoreOptions role_options{
        .connection_string = role_dsn, .schema = schema, .auto_create_schema = false, .connection_pool_size = 1U};
    a2a::server::stores::PostgresPushNotificationStore role_push_store(role_options);

    a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
    const auto created = role_push_store.CreateOrUpdateForTask(
        a2a::tests::store_conformance::MakeConfig(std::string(kSplitRoleTaskId), std::string(kSplitRoleConfigId)),
        task_store);

    ASSERT_TRUE(created.ok());
    const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
    EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
              2U);
    EXPECT_EQ(
        diagnostics
            .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
        1U);
    ExpectPushConfigPresent(owner_push_store, kSplitRoleTaskId, kSplitRoleConfigId);
  }
}

TEST(StoreConformanceTest, SplitRoleCreateRemovesConfigWhenDeletionWinsAfterPrecheck) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema("push_split_role_race");
  const a2a::server::stores::PostgresStoreOptions owner_options{
      .connection_string = dsn_value, .schema = schema, .connection_pool_size = 2U};
  a2a::server::stores::PostgresTaskStore task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_push_store(owner_options);
  AddPostgresTask(task_store, kSplitRoleTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);

  auto admin_connection = owner_push_store.AcquireConnectionForTesting();
  ASSERT_TRUE(admin_connection.ok());
  ScopedPostgresRole role(admin_connection.value().get(), MakePostgresTestRole(kSplitRolePrefix),
                          task_store.execution_identity().storage.database, schema);
  ASSERT_TRUE(role.Create().ok());
  const std::string role_dsn = BuildRoleDsn(dsn_value, role.role());
  ASSERT_FALSE(role_dsn.empty());

  const a2a::server::stores::PostgresStoreOptions role_options{
      .connection_string = role_dsn, .schema = schema, .auto_create_schema = false, .connection_pool_size = 1U};
  a2a::server::stores::PostgresPushNotificationStore role_push_store(role_options);
  std::future<SplitRoleCreateOutcome> create;
  {
    auto held_connection = role_push_store.AcquireConnectionForTesting();
    ASSERT_TRUE(held_connection.ok());
    create = std::async(std::launch::async, [&] {
      a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
      auto result = role_push_store.CreateOrUpdateForTask(
          a2a::tests::store_conformance::MakeConfig(std::string(kSplitRoleTaskId), std::string(kSplitRoleRaceConfigId)),
          task_store);
      return SplitRoleCreateOutcome{.result = std::move(result),
                                    .diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting()};
    });
    EXPECT_EQ(create.wait_for(kSplitRoleBlockedTimeout), std::future_status::timeout);
    ASSERT_TRUE(DeletePostgresTask(owner_push_store, owner_options, kSplitRoleTaskId).ok());
  }

  const auto outcome = create.get();
  ASSERT_FALSE(outcome.result.ok());
  EXPECT_EQ(outcome.result.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
  EXPECT_EQ(
      outcome.diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
      2U);
  EXPECT_EQ(outcome.diagnostics
                .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
            1U);
  EXPECT_EQ(outcome.diagnostics
                .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigDelete)],
            1U);
  ExpectPushConfigMissing(owner_push_store, kSplitRoleTaskId, kSplitRoleRaceConfigId);
}

TEST(StoreConformanceTest, LocalTaskDeletionPreservesExternalTaskStoreConfigs) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn_value,
                                                          .schema = MakePostgresTestSchema("push_provenance")};
  a2a::server::stores::PostgresTaskStore local_task_store(options);
  a2a::server::InMemoryTaskStore external_task_store;
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  AddPostgresTask(local_task_store, kProvenanceTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(external_task_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
                      std::string(kProvenanceTaskId), std::string(kPushListContextId), lf::a2a::v1::TASK_STATE_WORKING,
                      kOldTargetTaskTimestampSeconds))
                  .ok());

  ASSERT_TRUE(push_store
                  .CreateOrUpdateForTask(a2a::tests::store_conformance::MakeConfig(
                                             std::string(kProvenanceTaskId), std::string(kLocalProvenanceConfigId)),
                                         local_task_store)
                  .ok());
  ASSERT_TRUE(push_store
                  .CreateOrUpdateForTask(a2a::tests::store_conformance::MakeConfig(
                                             std::string(kProvenanceTaskId), std::string(kExternalProvenanceConfigId)),
                                         external_task_store)
                  .ok());

  ASSERT_TRUE(DeletePostgresTask(push_store, options, kProvenanceTaskId).ok());
  ExpectPushConfigMissing(push_store, kProvenanceTaskId, kLocalProvenanceConfigId);
  ExpectPushConfigPresent(push_store, kProvenanceTaskId, kExternalProvenanceConfigId);
}

TEST(StoreConformanceTest, ConflictUpdateUsesLatestTaskStoreProvenance) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema("push_provenance_transition")};
  a2a::server::stores::PostgresTaskStore local_task_store(options);
  a2a::server::InMemoryTaskStore external_task_store;
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  ASSERT_TRUE(external_task_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
                      std::string(kProvenanceTaskId), std::string(kPushListContextId), lf::a2a::v1::TASK_STATE_WORKING,
                      kOldTargetTaskTimestampSeconds))
                  .ok());
  AddPostgresTask(local_task_store, kProvenanceTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  const auto config = a2a::tests::store_conformance::MakeConfig(std::string(kProvenanceTaskId),
                                                                std::string(kTransitionProvenanceConfigId));

  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, external_task_store).ok());
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, local_task_store).ok());
  ASSERT_TRUE(DeletePostgresTask(push_store, options, kProvenanceTaskId).ok());
  ExpectPushConfigMissing(push_store, kProvenanceTaskId, kTransitionProvenanceConfigId);

  AddPostgresTask(local_task_store, kProvenanceTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, local_task_store).ok());
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, external_task_store).ok());
  ASSERT_TRUE(DeletePostgresTask(push_store, options, kProvenanceTaskId).ok());
  ExpectPushConfigPresent(push_store, kProvenanceTaskId, kTransitionProvenanceConfigId);
}

TEST(StoreConformanceTest, PostgresTaskDeleteTriggerUsesLeastPrivilegeSecurityDefiner) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema("push_least_privilege_delete");
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresTaskStore task_store(options);
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId),
                                                                            std::string(kAtomicCreateConfigId)))
                  .ok());
  const auto outcome = RunLeastPrivilegeTaskDelete(push_store, options);
  ExpectLeastPrivilegeDeleteOutcome(outcome);
}
#endif

}  // namespace
