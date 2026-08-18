// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <array>
#include <atomic>
#include <barrier>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

#ifdef A2A_ENABLE_POSTGRES_STORE
constexpr std::string_view kMissingTaskAwareListTaskId = "missing-task-aware-list";
constexpr std::string_view kMalformedTaskAwareListPageToken = "invalid-page-token";
constexpr int kInvalidTaskAwareListPageSize = -1;
constexpr int kValidTaskAwareListPageSize = 1;
constexpr auto kIndexPresenceSql = std::to_array("SELECT pg_catalog.to_regclass($1), pg_catalog.to_regclass($2)");
constexpr std::string_view kInspectPushIndexesOperation = "inspect push indexes";

void ExpectMissingTaskPrecedesTaskAwarePushListValidation(a2a::server::TaskAwarePushNotificationStore& push_store,
                                                          const a2a::server::TaskStore& task_store) {
  const auto invalid_page_size =
      push_store.ListForTask(kMissingTaskAwareListTaskId, kInvalidTaskAwareListPageSize, {}, task_store);
  ASSERT_FALSE(invalid_page_size.ok());
  EXPECT_EQ(invalid_page_size.error().protocol_code().value_or(std::string{}),
            a2a::core::protocol_codes::kTaskNotFound);

  const auto malformed_page_token = push_store.ListForTask(kMissingTaskAwareListTaskId, kValidTaskAwareListPageSize,
                                                           kMalformedTaskAwareListPageToken, task_store);
  ASSERT_FALSE(malformed_page_token.ok());
  EXPECT_EQ(malformed_page_token.error().protocol_code().value_or(std::string{}),
            a2a::core::protocol_codes::kTaskNotFound);
}
#endif

TEST(StoreConformanceTest, InMemoryTaskStore) {
  a2a::tests::store_conformance::RunTaskStoreConformance(
      [] { return std::make_unique<a2a::server::InMemoryTaskStore>(); });
}

TEST(StoreConformanceTest, InMemoryPushNotificationStore) {
  a2a::tests::store_conformance::RunPushNotificationStoreConformance(
      [] { return std::make_unique<a2a::server::InMemoryPushNotificationStore>(); });
}

#ifdef A2A_ENABLE_POSTGRES_STORE
constexpr auto kPostgresDsnEnvironmentVariable = std::to_array("A2A_TEST_POSTGRES_DSN");
constexpr std::string_view kPostgresAcquireFailureMessage = "test postgres acquire failure";
constexpr std::string_view kPostgresTestSchemaPrefix = "a2a_test_";
constexpr std::string_view kPostgresTestNameSeparator = "_";

[[nodiscard]] const char* GetPostgresDsn() { return std::getenv(kPostgresDsnEnvironmentVariable.data()); }

[[nodiscard]] a2a::core::Error MakePostgresAcquireFailureForTesting() {
  return a2a::core::Error::Internal(std::string(kPostgresAcquireFailureMessage));
}

void ExpectPostgresAcquireFailure(const a2a::core::Error& error) {
  EXPECT_EQ(error.code(), a2a::core::ErrorCode::kInternal);
  EXPECT_NE(error.message().find(kPostgresAcquireFailureMessage), std::string_view::npos);
}

[[nodiscard]] std::string MakePostgresTestSchema(std::string_view suffix) {
  const std::string ticks = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  std::string schema;
  schema.reserve(kPostgresTestSchemaPrefix.size() + ticks.size() + kPostgresTestNameSeparator.size() + suffix.size());
  schema.append(kPostgresTestSchemaPrefix);
  schema.append(ticks);
  schema.append(kPostgresTestNameSeparator);
  schema.append(suffix);
  return schema;
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
constexpr std::string_view kMissingAtomicCreateTaskId = "missing-atomic-create-task";
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
constexpr std::string_view kConninfoHostAddress = "hostaddr";
constexpr std::string_view kConninfoUser = "user";
constexpr std::string_view kConninfoPort = "port";
constexpr std::string_view kConninfoApplicationName = "application_name";
constexpr std::string_view kConninfoTargetSessionAttributes = "target_session_attrs";
constexpr std::string_view kConninfoOptions = "options";
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
constexpr std::string_view kIdentityHost = "database.example";
constexpr std::string_view kOtherIdentityHost = "replica.example";
constexpr std::string_view kIdentityHostAddress = "192.0.2.10";
constexpr std::string_view kOtherIdentityHostAddress = "192.0.2.11";
constexpr std::string_view kIdentityPort = "5432";
constexpr std::string_view kOtherIdentityPort = "5433";
constexpr std::string_view kIdentityDatabase = "a2a";
constexpr std::string_view kOtherIdentityDatabase = "other";
constexpr std::string_view kIdentitySchemaName = "public";
constexpr std::string_view kOtherIdentitySchemaName = "tenant";
constexpr std::string_view kIdentityTargetSessionAttributes = "read-write";
constexpr std::string_view kOtherIdentityTargetSessionAttributes = "any";
constexpr std::string_view kIdentityPrimaryTargetSessionAttributes = "primary";
constexpr std::string_view kIdentityMultiHost = "primary.example,standby.example";
constexpr std::string_view kReorderedIdentityMultiHost = "standby.example,primary.example";
constexpr std::string_view kIdentityMultiPort = "5432,5432";
constexpr std::string_view kIdentityStorageAuthorityId = "storage-authority-a";
constexpr std::string_view kOtherIdentityStorageAuthorityId = "storage-authority-b";
constexpr std::string_view kSessionPolicySchemaSuffix = "push_list_session_policy";
constexpr std::string_view kSessionPolicyName = "a2a_task_tenant_policy";
constexpr std::string_view kSessionPolicySetting = "app.tenant";
constexpr std::string_view kSessionPolicyOptionPrefix = "-c ";
constexpr std::string_view kSessionPolicyVisibleTenant = "tenant-a";
constexpr std::string_view kSessionPolicyHiddenTenant = "tenant-b";
constexpr std::string_view kSessionPolicyTaskId = "session-policy-task";
constexpr std::string_view kSessionPolicyConfigId = "session-policy-config";
constexpr int kSessionPolicyExpectedConfigCount = 1;
constexpr int kUnboundedPushListPageSize = 0;
constexpr std::string_view kInstallSessionPolicyOperation = "install task session-context row security policy";
constexpr std::string_view kDropSessionPolicyOperation = "drop task session-context row security policy";
constexpr std::string_view kSharedPoolTaskSchemaSuffix = "shared_pool_task_authority";
constexpr std::string_view kSharedPoolPushSchemaSuffix = "shared_pool_push_authority";
constexpr std::string_view kUncertainAuthoritySchemaSuffix = "push_authority_uncertain";
constexpr std::string_view kExternalTaskAuthoritySchemaSuffix = "task_authority_external";
constexpr std::string_view kExternalPushAuthoritySchemaSuffix = "push_authority_external";
constexpr std::string_view kExplicitExternalAuthoritySchemaSuffix = "push_authority_explicit_external";
constexpr std::string_view kAuthorityTaskId = "authority-task";
constexpr std::string_view kAuthorityConfigId = "authority-config";
constexpr std::string_view kPostgresDsnMissingSkipMessage = "A2A_TEST_POSTGRES_DSN is not set";
constexpr std::string_view kNonDefaultPortSkipMessage = "PostgreSQL test DSN does not use the default port";
constexpr auto kPgPortEnvironmentVariable = std::to_array("PGPORT");
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
constexpr std::string_view kGrantTaskSelectOperation = "grant read-only task access";
constexpr std::string_view kReadOnlyTaskRoleSchemaSuffix = "push_read_only_task_role";
constexpr std::string_view kTaskLockProbeRolePrefix = "a2a_push_lock_probe_";
constexpr std::string_view kTaskLockPrivilegeSchemaSuffix = "push_lock_privilege";
constexpr std::string_view kReadOnlyTaskRoleSetupOperation = "set up read-only task role";
constexpr std::string_view kInsufficientPrivilegeSqlState = "42501";
constexpr std::string_view kRetainTaskFunctionName = "zz_a2a_retain_task";
constexpr std::string_view kRetainTaskTriggerName = "zz_a2a_retain_task_trigger";
constexpr std::string_view kInstallRetainTaskTriggerOperation = "install task retention trigger";
constexpr std::string_view kSuppressedDeleteSchemaSuffix = "push_suppressed_task_delete";
constexpr std::string_view kExternalMigrationSchemaSuffix = "push_external_schema_migration";
constexpr std::string_view kManagedValidationSchemaSuffix = "push_managed_validation";
constexpr std::string_view kMissingCleanupTriggerSchemaSuffix = "push_external_schema_missing_cleanup";
constexpr std::string_view kLegacyForeignKeySchemaSuffix = "push_schema_legacy_fk";
constexpr std::string_view kCleanupImplementationSchemaSuffix = "push_schema_cleanup_impl";
constexpr std::string_view kCleanupIdentifierCaseSchemaSuffix = "PushSchemaCleanupCase";
constexpr std::string_view kLockImplementationSchemaSuffix = "push_schema_lock_impl";
constexpr std::string_view kCleanupVersionSchemaSuffix = "push_schema_cleanup_ver";
constexpr std::string_view kLockPublicExecuteSchemaSuffix = "push_schema_lock_public";
constexpr std::string_view kCleanupPublicExecuteSchemaSuffix = "push_schema_cleanup_public";
constexpr std::string_view kCleanupTriggerWhenSchemaSuffix = "push_schema_cleanup_when";
constexpr std::string_view kProvenanceTypeSchemaSuffix = "push_schema_prov_type";
constexpr std::string_view kProvenanceNotNullSchemaSuffix = "push_schema_prov_not_null";
constexpr std::string_view kProvenanceDefaultSchemaSuffix = "push_schema_prov_default";
constexpr std::string_view kLockOwnerPrivilegeSchemaSuffix = "push_schema_lock_owner";
constexpr std::string_view kCleanupOwnerPrivilegeSchemaSuffix = "push_schema_cleanup_owner";
constexpr std::string_view kCleanupOwnerRowLevelSecuritySchemaSuffix = "push_schema_cleanup_owner_rls";
constexpr std::string_view kPushOnlyExternalSchemaSuffix = "push_external_push_only";
constexpr std::string_view kPushOnlyNoTaskAwareSchemaSuffix = "push_external_no_task_aware";
constexpr std::string_view kListValidationOrderSchemaSuffix = "push_list_validation_order";
constexpr std::string_view kClearPushMigrationOperation = "simulate pre-migration push schema";
constexpr std::string_view kMutateManagedPushSchemaOperation = "mutate externally managed push schema";
constexpr std::string_view kDropTaskTableOperation = "drop task table for push-only schema";
constexpr std::string_view kRowLevelSecuritySchemaSuffix = "push_task_rls";
constexpr std::string_view kEnableTaskRowLevelSecurityOperation = "enable task row-level security";
constexpr std::string_view kEnablePushRowLevelSecurityOperation = "enable push row-level security";
constexpr std::string_view kExpectedTaskRowLevelSecurityError =
    "PostgreSQL task-aware push configuration does not support row-level security on a2a_tasks";
constexpr std::string_view kDropPushCleanupTriggerOperation = "drop push cleanup trigger";
constexpr std::string_view kSecurityDefinerOwnerRolePrefix = "a2a_push_security_owner_";
constexpr std::string_view kGrantSchemaCreateOperation = "grant schema create to security definer owner";
constexpr std::string_view kAlterSecurityDefinerOwnerOperation = "alter security definer owner";
constexpr std::string_view kReassignSecurityDefinerOwnerOperation = "restore security definer owner";
constexpr std::string_view kRevokePushDeleteOperation = "revoke push delete from security definer owner";
constexpr std::string_view kRevokeLocalTaskAwarePrivilegesOperation = "revoke local task-aware push privileges";
constexpr std::string_view kOutdatedManagedPushSchemaAcceptedMessage =
    "outdated externally managed PostgreSQL schema was accepted";
constexpr std::string_view kSplitRolePassword = "a2a_split_role_password";
constexpr std::string_view kSplitRoleExternalUrl = "https://example.test/split-role-external";
constexpr std::string_view kDirectGetClassificationSchemaSuffix = "push_direct_get_classification";
constexpr std::string_view kMismatchedPoolEndpoint = "mismatched-postgres-endpoint";
constexpr std::size_t kIdentityValidationPoolSize = 2U;
constexpr std::size_t kNoPostgresCommandCount = 0U;
constexpr std::size_t kSinglePostgresCommandCount = 1U;
constexpr std::size_t kSinglePushConfigGetCommandCount = 1U;
constexpr std::size_t kMissingPushConfigGetCommandCount = 1U;

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

[[nodiscard]] std::string LibpqValue(const char* value) {
  return value == nullptr ? std::string{} : std::string(value);
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

[[nodiscard]] std::string BuildEquivalentKeywordDsn(std::string_view dsn, bool include_port = true,
                                                    std::string_view target_session_attributes = {}) {
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
  AppendKeywordValue(equivalent, kConninfoHostAddress, ConninfoValue(options.get(), kConninfoHostAddress));
  AppendKeywordValue(equivalent, kConninfoUser, ConninfoValue(options.get(), kConninfoUser));
  if (include_port) {
    AppendKeywordValue(equivalent, kConninfoPort, ConninfoValue(options.get(), kConninfoPort));
  }
  AppendKeywordValue(equivalent, kConninfoApplicationName, kIdentityApplicationName);
  if (target_session_attributes.empty()) {
    AppendKeywordValue(equivalent, kConninfoTargetSessionAttributes,
                       ConninfoValue(options.get(), kConninfoTargetSessionAttributes));
  } else {
    AppendKeywordValue(equivalent, kConninfoTargetSessionAttributes, target_session_attributes);
  }
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
  const std::string ticks = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  std::string role;
  role.reserve(prefix.size() + ticks.size());
  role.append(prefix);
  role.append(ticks);
  return role;
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
  AppendKeywordValue(role_dsn, kConninfoHostAddress, ConninfoValue(options.get(), kConninfoHostAddress));
  AppendKeywordValue(role_dsn, kConninfoUser, role);
  AppendKeywordValue(role_dsn, kConninfoPort, ConninfoValue(options.get(), kConninfoPort));
  AppendKeywordValue(role_dsn, kConninfoApplicationName, kIdentityApplicationName);
  AppendKeywordValue(role_dsn, kConninfoTargetSessionAttributes,
                     ConninfoValue(options.get(), kConninfoTargetSessionAttributes));
  return role_dsn;
}

[[nodiscard]] std::string BuildRoleDsnWithStartupOption(std::string_view dsn, std::string_view role,
                                                        std::string_view startup_option) {
  std::string role_dsn = BuildRoleDsn(dsn, role);
  if (!role_dsn.empty()) {
    AppendKeywordValue(role_dsn, kConninfoOptions, startup_option);
  }
  return role_dsn;
}

[[nodiscard]] std::string BuildSplitRoleSetupSql(std::string_view role, std::string_view database,
                                                 std::string_view schema) {
  const std::string quoted_role = a2a::server::stores::QuoteSqlIdentifier(role);
  const std::string quoted_database = a2a::server::stores::QuoteSqlIdentifier(database);
  const std::string quoted_schema = a2a::server::stores::QuoteSqlIdentifier(schema);
  const std::string task_table = a2a::server::stores::TaskTable(schema);
  const std::string push_table = a2a::server::stores::PushTable(schema);
  const std::string push_sequence =
      a2a::server::stores::QualifiedSqlIdentifier(schema, a2a::server::stores::kPushCreatedSequenceName);
  const std::string task_lock_function = a2a::server::stores::TaskPushConfigLockFunction(schema);
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
  sql.append("; GRANT SELECT ON ");
  sql.append(task_table);
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
  sql.append("; GRANT EXECUTE ON FUNCTION ");
  sql.append(task_lock_function);
  sql.append("(text) TO ");
  sql.append(quoted_role);
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildGrantTaskSelectSql(std::string_view role, std::string_view task_table) {
  std::string sql = "GRANT SELECT ON ";
  sql.append(task_table);
  sql.append(" TO ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(role));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildReadOnlyTaskRoleSetupSql(std::string_view role, std::string_view schema,
                                                        std::string_view task_table) {
  const std::string quoted_role = a2a::server::stores::QuoteSqlIdentifier(role);
  std::string sql = "CREATE ROLE ";
  sql.append(quoted_role);
  sql.append(" NOLOGIN; GRANT USAGE ON SCHEMA ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(schema));
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.append("; GRANT SELECT ON ");
  sql.append(task_table);
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildTaskPushConfigLockSql(std::string_view schema) {
  std::string sql = "SELECT ";
  sql.append(a2a::server::stores::TaskPushConfigLockFunction(schema));
  sql.append("($1)");
  return sql;
}

[[nodiscard]] bool IsInsufficientPrivilege(PGresult* result) {
  if (result == nullptr || PQresultStatus(result) != PGRES_FATAL_ERROR) {
    return false;
  }
  const char* sql_state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
  return sql_state != nullptr && std::string_view(sql_state) == kInsufficientPrivilegeSqlState;
}

[[nodiscard]] a2a::core::Result<bool> RunReadOnlyTaskLockAttempt(
    a2a::server::stores::PostgresPushNotificationStore& push_store,
    const a2a::server::stores::PostgresStoreOptions& options) {
  auto connection = push_store.AcquireConnectionForTesting();
  if (!connection.ok()) {
    return connection.error();
  }
  const std::string role = MakePostgresTestRole(kTaskLockProbeRolePrefix);
  const std::string task_table = a2a::server::stores::TaskTable(options.schema);
  const auto setup = a2a::server::stores::Exec(connection.value().get(),
                                               BuildReadOnlyTaskRoleSetupSql(role, options.schema, task_table),
                                               kReadOnlyTaskRoleSetupOperation);
  if (!setup.ok()) {
    return setup.error();
  }
  const auto switched =
      a2a::server::stores::Exec(connection.value().get(), BuildSetRoleSql(role), kRoleSwitchOperation);
  if (!switched.ok()) {
    return switched.error();
  }
  const std::string task_id(kAtomicCreateTaskId);
  const std::array<const char*, 1> values = {task_id.c_str()};
  const std::string lock_sql = BuildTaskPushConfigLockSql(options.schema);
  a2a::server::stores::PgResult lock_result(PQexecParams(connection.value().get(), lock_sql.c_str(),
                                                         static_cast<int>(values.size()), nullptr, values.data(),
                                                         nullptr, nullptr, 0));
  const bool denied = IsInsufficientPrivilege(lock_result.get());
  const auto reset =
      a2a::server::stores::Exec(connection.value().get(), std::string(kResetRoleSql), kRoleResetOperation);
  if (!reset.ok()) {
    return reset.error();
  }
  const auto cleanup =
      a2a::server::stores::Exec(connection.value().get(), BuildDropRoleSql(role), kRoleCleanupOperation);
  if (!cleanup.ok()) {
    return cleanup.error();
  }
  return denied;
}

[[nodiscard]] std::string BuildRetainTaskTriggerSql(std::string_view schema) {
  const std::string function = a2a::server::stores::QualifiedSqlIdentifier(schema, kRetainTaskFunctionName);
  const std::string trigger = a2a::server::stores::QuoteSqlIdentifier(kRetainTaskTriggerName);
  const std::string task_table = a2a::server::stores::TaskTable(schema);
  std::string sql = "CREATE OR REPLACE FUNCTION ";
  sql.append(function);
  sql.append("() RETURNS trigger LANGUAGE plpgsql AS $a2a$ BEGIN RETURN NULL; END $a2a$; DROP TRIGGER IF EXISTS ");
  sql.append(trigger);
  sql.append(" ON ");
  sql.append(task_table);
  sql.append("; CREATE TRIGGER ");
  sql.append(trigger);
  sql.append(" BEFORE DELETE ON ");
  sql.append(task_table);
  sql.append(" FOR EACH ROW EXECUTE FUNCTION ");
  sql.append(function);
  sql.append("();");
  return sql;
}

[[nodiscard]] std::string BuildClearPushMigrationSql(std::string_view schema) {
  std::string sql = "COMMENT ON FUNCTION ";
  sql.append(a2a::server::stores::TaskPushConfigLockFunction(schema));
  sql.append("(text) IS NULL;");
  return sql;
}

[[nodiscard]] std::string DeleteTaskPushConfigsFunction(std::string_view schema) {
  return a2a::server::stores::QualifiedSqlIdentifier(schema, a2a::server::stores::kDeleteTaskPushConfigsFunction);
}

[[nodiscard]] std::string BuildClearCleanupMigrationSql(std::string_view schema) {
  std::string sql = "COMMENT ON FUNCTION ";
  sql.append(DeleteTaskPushConfigsFunction(schema));
  sql.append("() IS NULL;");
  return sql;
}

[[nodiscard]] std::string BuildRestoreLegacyTaskForeignKeySql(std::string_view schema) {
  std::string sql = "ALTER TABLE ";
  sql.append(a2a::server::stores::PushTable(schema));
  sql.append(" ADD CONSTRAINT ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(a2a::server::stores::kPushConfigsTaskForeignKey));
  sql.append(" FOREIGN KEY (task_id) REFERENCES ");
  sql.append(a2a::server::stores::TaskTable(schema));
  sql.append("(id);");
  return sql;
}

[[nodiscard]] std::string BuildNoOpCleanupFunctionSql(std::string_view schema) {
  std::string sql = "CREATE OR REPLACE FUNCTION ";
  sql.append(DeleteTaskPushConfigsFunction(schema));
  sql.append(
      "() RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog "
      "AS $a2a$ BEGIN RETURN OLD; END $a2a$;");
  return sql;
}

[[nodiscard]] std::string BuildWrongCaseCleanupFunctionSql(std::string_view schema) {
  std::string wrong_schema(schema);
  for (char& symbol : wrong_schema) {
    symbol = static_cast<char>(std::tolower(static_cast<unsigned char>(symbol)));
  }
  std::string sql = "CREATE OR REPLACE FUNCTION ";
  sql.append(DeleteTaskPushConfigsFunction(schema));
  sql.append("() RETURNS trigger LANGUAGE plpgsql SECURITY DEFINER SET search_path = pg_catalog AS $a2a$ BEGIN ");
  sql.append("DELETE FROM ");
  sql.append(a2a::server::stores::PushTable(wrong_schema));
  sql.append(" WHERE task_id = OLD.id AND local_postgres_task; RETURN OLD; END $a2a$; COMMENT ON FUNCTION ");
  sql.append(DeleteTaskPushConfigsFunction(schema));
  sql.append("() IS '");
  sql.append(a2a::server::stores::kTaskPushConfigMigrationId);
  sql.append("';");
  return sql;
}

[[nodiscard]] std::string BuildNoOpTaskLockFunctionSql(std::string_view schema) {
  const std::string function = a2a::server::stores::TaskPushConfigLockFunction(schema);
  std::string sql = "CREATE OR REPLACE FUNCTION ";
  sql.append(function);
  sql.append(
      "(requested_task_id text) RETURNS boolean LANGUAGE plpgsql VOLATILE SECURITY DEFINER "
      "SET search_path = pg_catalog AS $a2a$ BEGIN RETURN TRUE; END $a2a$; COMMENT ON FUNCTION ");
  sql.append(function);
  sql.append("(text) IS '");
  sql.append(a2a::server::stores::kTaskPushConfigMigrationId);
  sql.append("';");
  return sql;
}

[[nodiscard]] std::string BuildDropTaskAwarePushSchemaSql(std::string_view schema) {
  std::string sql = "DROP TRIGGER ";
  sql.append(a2a::server::stores::QuoteSqlIdentifier(a2a::server::stores::kDeleteTaskPushConfigsTrigger));
  sql.append(" ON ");
  sql.append(a2a::server::stores::TaskTable(schema));
  sql.append("; DROP FUNCTION ");
  sql.append(a2a::server::stores::TaskPushConfigLockFunction(schema));
  sql.append("(text); DROP FUNCTION ");
  sql.append(DeleteTaskPushConfigsFunction(schema));
  sql.append("();");
  return sql;
}

[[nodiscard]] std::string BuildDropTaskTableSql(std::string_view schema) {
  std::string sql = "DROP TABLE ";
  sql.append(a2a::server::stores::TaskTable(schema));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildGrantPublicLockExecuteSql(std::string_view schema) {
  std::string sql = "GRANT EXECUTE ON FUNCTION ";
  sql.append(a2a::server::stores::TaskPushConfigLockFunction(schema));
  sql.append("(text) TO PUBLIC;");
  return sql;
}

[[nodiscard]] std::string BuildGrantPublicCleanupExecuteSql(std::string_view schema) {
  std::string sql = "GRANT EXECUTE ON FUNCTION ";
  sql.append(DeleteTaskPushConfigsFunction(schema));
  sql.append("() TO PUBLIC;");
  return sql;
}

[[nodiscard]] std::string BuildCleanupTriggerWithWhenSql(std::string_view schema) {
  const std::string trigger =
      a2a::server::stores::QuoteSqlIdentifier(a2a::server::stores::kDeleteTaskPushConfigsTrigger);
  const std::string task_table = a2a::server::stores::TaskTable(schema);
  std::string sql = "DROP TRIGGER ";
  sql.append(trigger);
  sql.append(" ON ");
  sql.append(task_table);
  sql.append("; CREATE TRIGGER ");
  sql.append(trigger);
  sql.append(" AFTER DELETE ON ");
  sql.append(task_table);
  sql.append(" FOR EACH ROW WHEN (OLD.id IS NOT NULL) EXECUTE FUNCTION ");
  sql.append(DeleteTaskPushConfigsFunction(schema));
  sql.append("();");
  return sql;
}

[[nodiscard]] std::string BuildWrongProvenanceTypeSql(std::string_view schema) {
  const std::string push_table = a2a::server::stores::PushTable(schema);
  std::string sql = "ALTER TABLE ";
  sql.append(push_table);
  sql.append(" DROP COLUMN local_postgres_task; ALTER TABLE ");
  sql.append(push_table);
  sql.append(" ADD COLUMN local_postgres_task TEXT NOT NULL DEFAULT 'false';");
  return sql;
}

[[nodiscard]] std::string BuildNullableProvenanceSql(std::string_view schema) {
  std::string sql = "ALTER TABLE ";
  sql.append(a2a::server::stores::PushTable(schema));
  sql.append(" ALTER COLUMN local_postgres_task DROP NOT NULL;");
  return sql;
}

[[nodiscard]] std::string BuildTrueProvenanceDefaultSql(std::string_view schema) {
  std::string sql = "ALTER TABLE ";
  sql.append(a2a::server::stores::PushTable(schema));
  sql.append(" ALTER COLUMN local_postgres_task SET DEFAULT TRUE;");
  return sql;
}

[[nodiscard]] std::string BuildDropPushCleanupTriggerSql(std::string_view schema) {
  std::string sql = "DROP TRIGGER ";
  sql.append(a2a::server::stores::QuoteSqlIdentifier(a2a::server::stores::kDeleteTaskPushConfigsTrigger));
  sql.append(" ON ");
  sql.append(a2a::server::stores::TaskTable(schema));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildEnableTaskRowLevelSecuritySql(std::string_view schema) {
  std::string sql = "ALTER TABLE ";
  sql.append(a2a::server::stores::TaskTable(schema));
  sql.append(" ENABLE ROW LEVEL SECURITY;");
  return sql;
}

[[nodiscard]] std::string BuildTaskSessionPolicySql(std::string_view schema, std::string_view role) {
  const std::string task_table = a2a::server::stores::TaskTable(schema);
  std::string sql = "ALTER TABLE ";
  sql.append(task_table);
  sql.append(" ENABLE ROW LEVEL SECURITY; CREATE POLICY ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(kSessionPolicyName));
  sql.append(" ON ");
  sql.append(task_table);
  sql.append(" FOR SELECT TO ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(role));
  sql.append(" USING (pg_catalog.current_setting('");
  sql.append(kSessionPolicySetting);
  sql.append("', true) = '");
  sql.append(kSessionPolicyVisibleTenant);
  sql.append("');");
  return sql;
}

[[nodiscard]] std::string BuildDropTaskSessionPolicySql(std::string_view schema) {
  std::string sql = "DROP POLICY ";
  sql.append(a2a::server::stores::QuoteSqlIdentifier(kSessionPolicyName));
  sql.append(" ON ");
  sql.append(a2a::server::stores::TaskTable(schema));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildSessionPolicyStartupOption(std::string_view tenant) {
  std::string option(kSessionPolicyOptionPrefix);
  option.append(kSessionPolicySetting);
  option.push_back('=');
  option.append(tenant);
  return option;
}

[[nodiscard]] std::string BuildEnablePushRowLevelSecuritySql(std::string_view schema) {
  std::string sql = "ALTER TABLE ";
  sql.append(a2a::server::stores::PushTable(schema));
  sql.append(" ENABLE ROW LEVEL SECURITY;");
  return sql;
}

[[nodiscard]] std::string BuildGrantSchemaCreateSql(std::string_view role, std::string_view schema) {
  const std::string quoted_role = a2a::server::stores::QuoteSqlIdentifier(role);
  std::string sql = "GRANT ";
  sql.append(quoted_role);
  sql.append(" TO CURRENT_USER; GRANT CREATE ON SCHEMA ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(schema));
  sql.append(" TO ");
  sql.append(quoted_role);
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildAlterTaskLockFunctionOwnerSql(std::string_view schema, std::string_view role) {
  std::string sql = "ALTER FUNCTION ";
  sql.append(a2a::server::stores::TaskPushConfigLockFunction(schema));
  sql.append("(text) OWNER TO ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(role));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildAlterCleanupFunctionOwnerSql(std::string_view schema, std::string_view role) {
  std::string sql = "ALTER FUNCTION ";
  sql.append(DeleteTaskPushConfigsFunction(schema));
  sql.append("() OWNER TO ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(role));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildReassignRoleObjectsSql(std::string_view role) {
  std::string sql = "REASSIGN OWNED BY ";
  sql.append(a2a::server::stores::QuoteSqlIdentifier(role));
  sql.append(" TO CURRENT_USER;");
  return sql;
}

[[nodiscard]] std::string BuildRevokePushDeleteSql(std::string_view schema, std::string_view role) {
  std::string sql = "REVOKE DELETE ON ";
  sql.append(a2a::server::stores::PushTable(schema));
  sql.append(" FROM ");
  sql.append(a2a::server::stores::QuoteSqlIdentifier(role));
  sql.push_back(';');
  return sql;
}

[[nodiscard]] std::string BuildRevokeLocalTaskAwarePrivilegesSql(std::string_view schema, std::string_view role) {
  const std::string quoted_role = a2a::server::stores::QuoteSqlIdentifier(role);
  std::string sql = "REVOKE SELECT ON ";
  sql.append(a2a::server::stores::TaskTable(schema));
  sql.append(" FROM ");
  sql.append(quoted_role);
  sql.append("; REVOKE EXECUTE ON FUNCTION ");
  sql.append(a2a::server::stores::TaskPushConfigLockFunction(schema));
  sql.append("(text) FROM ");
  sql.append(quoted_role);
  sql.push_back(';');
  return sql;
}

void ExpectManagedPushSchemaRejected(const a2a::server::stores::PostgresStoreOptions& options) {
  try {
    (void)a2a::server::stores::PostgresPushNotificationStore(options);
    FAIL() << kOutdatedManagedPushSchemaAcceptedMessage;
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string_view(error.what()).find(a2a::server::stores::kTaskPushConfigMigrationId),
              std::string_view::npos);
  }
}

using ManagedPushSchemaMutationBuilder = std::string (*)(std::string_view);

void ExpectManagedPushSchemaMutationRejected(std::string_view schema_suffix,
                                             ManagedPushSchemaMutationBuilder build_mutation) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema(schema_suffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresPushNotificationStore owner_store(owner_options);
  auto connection = owner_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  ASSERT_TRUE(
      a2a::server::stores::Exec(connection.value().get(), build_mutation(schema), kMutateManagedPushSchemaOperation)
          .ok());
  const a2a::server::stores::PostgresStoreOptions managed_options{
      .connection_string = dsn_value, .schema = schema, .auto_create_schema = false};
  ExpectManagedPushSchemaRejected(managed_options);
}

void ExpectPushConfigValidationError(const a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>& result) {
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_FALSE(result.error().protocol_code().has_value());
}

void ExpectPushConfigTaskNotFound(const a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>& result) {
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
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

enum class SecurityDefinerOwnerCase : std::uint8_t { kTaskLock, kCleanup };

void ExpectPostgresExecOk(PGconn* connection, const std::string& sql, std::string_view operation) {
  ASSERT_TRUE(a2a::server::stores::Exec(connection, sql, operation).ok());
}

void ExpectSecurityDefinerOwnerPrivilegesRequired(std::string_view schema_suffix, SecurityDefinerOwnerCase owner_case) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema(schema_suffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresTaskStore task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_store(owner_options);
  auto connection = owner_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  ScopedPostgresRole role(connection.value().get(), MakePostgresTestRole(kSecurityDefinerOwnerRolePrefix),
                          task_store.execution_identity().storage.database, schema);
  const auto role_created = role.Create();
  ASSERT_TRUE(role_created.ok());
  ExpectPostgresExecOk(connection.value().get(), BuildGrantSchemaCreateSql(role.role(), schema),
                       kGrantSchemaCreateOperation);
  if (owner_case == SecurityDefinerOwnerCase::kCleanup) {
    ExpectPostgresExecOk(connection.value().get(), BuildRevokePushDeleteSql(schema, role.role()),
                         kRevokePushDeleteOperation);
  }
  const std::string alter_owner_sql = owner_case == SecurityDefinerOwnerCase::kTaskLock
                                          ? BuildAlterTaskLockFunctionOwnerSql(schema, role.role())
                                          : BuildAlterCleanupFunctionOwnerSql(schema, role.role());
  ExpectPostgresExecOk(connection.value().get(), alter_owner_sql, kAlterSecurityDefinerOwnerOperation);
  const a2a::server::stores::PostgresStoreOptions managed_options{
      .connection_string = dsn_value, .schema = schema, .auto_create_schema = false};
  ExpectManagedPushSchemaRejected(managed_options);
  ExpectPostgresExecOk(connection.value().get(), BuildReassignRoleObjectsSql(role.role()),
                       kReassignSecurityDefinerOwnerOperation);
}

void ExpectCleanupOwnerRowLevelSecurityRejected() {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  const std::string schema = MakePostgresTestSchema(kCleanupOwnerRowLevelSecuritySchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresTaskStore task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_store(owner_options);
  auto connection = owner_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  ScopedPostgresRole role(connection.value().get(), MakePostgresTestRole(kSecurityDefinerOwnerRolePrefix),
                          task_store.execution_identity().storage.database, schema);
  ASSERT_TRUE(role.Create().ok());
  ExpectPostgresExecOk(connection.value().get(), BuildGrantSchemaCreateSql(role.role(), schema),
                       kGrantSchemaCreateOperation);
  ExpectPostgresExecOk(connection.value().get(), BuildAlterCleanupFunctionOwnerSql(schema, role.role()),
                       kAlterSecurityDefinerOwnerOperation);
  ExpectPostgresExecOk(connection.value().get(), BuildEnablePushRowLevelSecuritySql(schema),
                       kEnablePushRowLevelSecurityOperation);
  const a2a::server::stores::PostgresStoreOptions validated_options{
      .connection_string = dsn_value, .schema = schema, .auto_create_schema = false};
  ExpectManagedPushSchemaRejected(validated_options);
  const auto reassigned = a2a::server::stores::Exec(connection.value().get(), BuildReassignRoleObjectsSql(role.role()),
                                                    kReassignSecurityDefinerOwnerOperation);
  ASSERT_TRUE(reassigned.ok());
}

[[nodiscard]] std::string BuildDeleteByTaskIdSql(std::string_view table, std::string_view id_column,
                                                 std::string_view task_id);

void AddPostgresTask(a2a::server::stores::PostgresTaskStore& store, std::string_view task_id,
                     std::string_view context_id, lf::a2a::v1::TaskState state, int timestamp_seconds);

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

struct SplitRoleDeletionFirstOutcome final {
  std::future_status create_wait_status;
  SplitRoleCreateOutcome create;
};

[[nodiscard]] a2a::core::Result<SplitRoleDeletionFirstOutcome> RunSplitRoleDeletionFirstScenario(
    a2a::server::stores::PostgresPushNotificationStore& role_push_store,
    a2a::server::stores::PostgresTaskStore& task_store,
    a2a::server::stores::PostgresPushNotificationStore& owner_push_store,
    const a2a::server::stores::PostgresStoreOptions& owner_options) {
  std::future<SplitRoleCreateOutcome> create;
  std::future_status create_wait_status = std::future_status::deferred;
  a2a::core::Result<void> committed;
  {
    auto deletion_connection = owner_push_store.AcquireConnectionForTesting();
    if (!deletion_connection.ok()) {
      return deletion_connection.error();
    }
    a2a::server::stores::Transaction deletion_transaction(deletion_connection.value().get());
    const auto begun = deletion_transaction.Begin();
    if (!begun.ok()) {
      return begun.error();
    }
    const auto deleted = a2a::server::stores::Exec(
        deletion_connection.value().get(),
        BuildDeleteByTaskIdSql(a2a::server::stores::TaskTable(owner_options.schema), kTaskIdColumn, kSplitRoleTaskId),
        kHoldConcurrentDeleteOperation);
    if (!deleted.ok()) {
      return deleted.error();
    }

    create = std::async(std::launch::async, [&] {
      a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
      auto result = role_push_store.CreateOrUpdateForTask(
          a2a::tests::store_conformance::MakeConfig(std::string(kSplitRoleTaskId), std::string(kSplitRoleRaceConfigId)),
          task_store);
      return SplitRoleCreateOutcome{.result = std::move(result),
                                    .diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting()};
    });
    create_wait_status = create.wait_for(kConcurrentCreateBlockedTimeout);
    committed = deletion_transaction.Commit();
  }
  auto create_outcome = create.get();
  if (!committed.ok()) {
    return committed.error();
  }
  return SplitRoleDeletionFirstOutcome{.create_wait_status = create_wait_status, .create = std::move(create_outcome)};
}

void ExpectSplitRoleCreateDiagnostics(const a2a::server::stores::PostgresOperationDiagnostics& diagnostics) {
  EXPECT_EQ(
      diagnostics
          .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kConnectionAcquireWait)],
      kSinglePostgresCommandCount);
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            kNoPostgresCommandCount);
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      kSinglePostgresCommandCount);
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTransactionBegin)],
      kNoPostgresCommandCount);
  EXPECT_EQ(diagnostics
                .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTransactionCommit)],
            kNoPostgresCommandCount);
}

void ExpectSplitRoleDeletionFirstCreateOutcome(const SplitRoleCreateOutcome& outcome) {
  ASSERT_FALSE(outcome.result.ok());
  EXPECT_EQ(outcome.result.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
  ExpectSplitRoleCreateDiagnostics(outcome.diagnostics);
}

void ExpectPushConfigUrl(a2a::server::stores::PostgresPushNotificationStore& store, std::string_view task_id,
                         std::string_view config_id, std::string_view expected_url) {
  const auto stored = store.Get(task_id, config_id);
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored.value().url(), expected_url);
}

void RunAndExpectSplitRoleDeletionFirstScenario(a2a::server::stores::PostgresPushNotificationStore& role_push_store,
                                                a2a::server::stores::PostgresTaskStore& task_store,
                                                a2a::server::stores::PostgresPushNotificationStore& owner_push_store,
                                                const a2a::server::stores::PostgresStoreOptions& owner_options) {
  auto external =
      a2a::tests::store_conformance::MakeConfig(std::string(kSplitRoleTaskId), std::string(kSplitRoleRaceConfigId));
  external.set_url(std::string(kSplitRoleExternalUrl));
  ASSERT_TRUE(owner_push_store.CreateOrUpdate(external).ok());

  const auto outcome = RunSplitRoleDeletionFirstScenario(role_push_store, task_store, owner_push_store, owner_options);
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome.value().create_wait_status, std::future_status::timeout);
  ExpectSplitRoleDeletionFirstCreateOutcome(outcome.value().create);
  ExpectPushConfigUrl(owner_push_store, kSplitRoleTaskId, kSplitRoleRaceConfigId, kSplitRoleExternalUrl);
}

void ExpectLocalTaskStoreProvenanceAfterConflict(a2a::server::stores::PostgresPushNotificationStore& push_store,
                                                 a2a::server::stores::PostgresTaskStore& local_task_store,
                                                 a2a::server::InMemoryTaskStore& external_task_store,
                                                 const a2a::server::stores::PostgresStoreOptions& options,
                                                 const lf::a2a::v1::TaskPushNotificationConfig& config) {
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, external_task_store).ok());
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, local_task_store).ok());
  ASSERT_TRUE(DeletePostgresTask(push_store, options, kProvenanceTaskId).ok());
  ExpectPushConfigMissing(push_store, kProvenanceTaskId, kTransitionProvenanceConfigId);
}

void ExpectExternalTaskStoreProvenanceAfterConflict(a2a::server::stores::PostgresPushNotificationStore& push_store,
                                                    a2a::server::stores::PostgresTaskStore& local_task_store,
                                                    a2a::server::InMemoryTaskStore& external_task_store,
                                                    const a2a::server::stores::PostgresStoreOptions& options,
                                                    const lf::a2a::v1::TaskPushNotificationConfig& config) {
  AddPostgresTask(local_task_store, kProvenanceTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, local_task_store).ok());
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, external_task_store).ok());
  ASSERT_TRUE(DeletePostgresTask(push_store, options, kProvenanceTaskId).ok());
  ExpectPushConfigPresent(push_store, kProvenanceTaskId, kTransitionProvenanceConfigId);
}

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
  SplitRoleCreateOutcome create;
  bool config_remains;
};

[[nodiscard]] a2a::core::Result<ConcurrentDeleteOutcome> RunConcurrentPushConfigDelete(
    a2a::server::stores::PostgresPushNotificationStore& push_store, a2a::server::stores::PostgresTaskStore& task_store,
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
    a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
    auto result = push_store.CreateOrUpdateForTask(
        a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId)),
        task_store);
    return SplitRoleCreateOutcome{
        .result = std::move(result),
        .diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting(),
    };
  });
  create_started.arrive_and_wait();
  const std::future_status wait_status = create.wait_for(kConcurrentCreateBlockedTimeout);
  const auto committed = deletion_transaction.Commit();
  if (!committed.ok()) {
    return committed.error();
  }
  auto create_outcome = create.get();
  const bool config_remains = push_store.Get(kAtomicCreateTaskId, kAtomicCreateConfigId).ok();
  return ConcurrentDeleteOutcome{
      .create_wait_status = wait_status, .create = std::move(create_outcome), .config_remains = config_remains};
}

void ExpectConcurrentDeleteOutcome(const a2a::core::Result<ConcurrentDeleteOutcome>& outcome) {
  ASSERT_TRUE(outcome.ok());
  EXPECT_EQ(outcome.value().create_wait_status, std::future_status::timeout);
  ASSERT_FALSE(outcome.value().create.result.ok());
  EXPECT_EQ(outcome.value().create.result.error().protocol_code().value_or(std::string{}),
            a2a::core::protocol_codes::kTaskNotFound);
  ExpectSplitRoleCreateDiagnostics(outcome.value().create.diagnostics);
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

void ExpectSinglePushConfigUpsertWithoutTaskGet() {
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      1U);
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            0U);
}

void ExpectTaskFirstPushConfigListCommands() {
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            kSinglePostgresCommandCount);
  EXPECT_EQ(
      diagnostics
          .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigListSelect)],
      kSinglePostgresCommandCount);
}

void ExpectPushConfigGetCommandCount(std::size_t expected_count) {
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigGet)],
      expected_count);
}

void ExpectDirectPushConfigPresentWithSingleCommand(a2a::server::stores::PostgresPushNotificationStore& push_store) {
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto fetched = push_store.Get(kMixedCreateTaskId, kMixedCreateConfigId);
  ASSERT_TRUE(fetched.ok());
  ExpectPushConfigGetCommandCount(kSinglePushConfigGetCommandCount);
}

void ExpectDirectPushConfigMissingConfig(a2a::server::stores::PostgresPushNotificationStore& push_store) {
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto missing = push_store.Get(kMixedCreateTaskId, kAtomicCreateConfigId);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_FALSE(missing.error().protocol_code().has_value());
  ExpectPushConfigGetCommandCount(kMissingPushConfigGetCommandCount);
}

void ExpectDirectPushConfigMissingTask(a2a::server::stores::PostgresPushNotificationStore& push_store) {
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto missing = push_store.Get(kAtomicCreateTaskId, kMixedCreateConfigId);
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().protocol_code().value_or(std::string{}), a2a::core::protocol_codes::kTaskNotFound);
  ExpectPushConfigGetCommandCount(kMissingPushConfigGetCommandCount);
}

void ExpectStoredLargePushConfig(const a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>& stored) {
  ASSERT_TRUE(stored.ok());
  EXPECT_EQ(stored.value().url(), kLargeConfigUpdatedUrl);
  EXPECT_EQ(stored.value().token().size(), kLargeConfigMetadataSize);
  EXPECT_EQ(stored.value().authentication().credentials().size(), kLargeConfigMetadataSize);
}

void ExpectUncertainAuthorityCreateRejected(const a2a::core::Result<lf::a2a::v1::TaskPushNotificationConfig>& created,
                                            const a2a::server::stores::PostgresOperationDiagnostics& diagnostics,
                                            const a2a::server::stores::PostgresPushNotificationStore& push_store) {
  ASSERT_FALSE(created.ok());
  EXPECT_EQ(created.error().message(), a2a::server::stores::kPostgresTaskAuthorityUncertainMessage);
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            kSinglePostgresCommandCount);
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      kNoPostgresCommandCount);
  EXPECT_FALSE(push_store.Get(kAuthorityTaskId, kAuthorityConfigId).ok());
}

constexpr std::string_view kCreateFirstSchemaSuffix = "push_create_first";
constexpr std::string_view kConflictingWriterSchemaSuffix = "push_conflicting_writer";
constexpr std::string_view kExternalConcurrentSchemaSuffix = "push_external_concurrent";
constexpr std::string_view kDatabaseLockCycleSchemaSuffix = "push_database_lock_cycle";
constexpr std::string_view kConcurrentFirstUpdateUrl = "https://example.test/concurrent-first";
constexpr std::string_view kConcurrentSecondUpdateUrl = "https://example.test/concurrent-second";
constexpr std::string_view kConcurrentCycleUpdateUrl = "https://example.test/concurrent-cycle";
constexpr std::string_view kConcurrentExternalUpdateUrl = "https://example.test/concurrent-external";
constexpr std::string_view kLockConcurrentPushRowOperation = "lock concurrent push config row";
constexpr std::string_view kCountWaitingPostgresLocksOperation = "count waiting postgres locks";
constexpr std::string_view kCountWaitingPostgresLocksMissingRowMessage =
    "count waiting postgres locks: query returned no row";
constexpr std::string_view kPostgresLockWaitTimeoutMessage = "timed out waiting for PostgreSQL lock waiter";
constexpr std::string_view kSetConcurrentStatementTimeoutOperation = "set concurrency test statement timeout";
constexpr std::string_view kConcurrentTaskDeleteOperation = "delete task during concurrent push update";
constexpr std::string_view kConcurrentStatementTimeoutSql = "SET statement_timeout = '5s'";
constexpr auto kCountWaitingPostgresLocksSql = std::to_array(
    "SELECT count(*)::text FROM pg_catalog.pg_stat_activity "
    "WHERE datname = current_database() AND usename = current_user "
    "AND application_name = $1 AND wait_event_type = 'Lock'");
constexpr std::string_view kPostgresDeadlockSqlState = "40P01";
constexpr std::string_view kPostgresDeadlockMessage = "deadlock detected";
constexpr auto kLockWaitPollInterval = std::chrono::milliseconds(10);
constexpr std::size_t kLockWaitPollAttempts = 300U;
constexpr std::size_t kConcurrencyTaskPoolSize = 1U;
constexpr std::size_t kCreateFirstPoolSize = 3U;
constexpr std::size_t kConflictingWriterPoolSize = 3U;
constexpr std::size_t kExternalConcurrentPoolSize = 3U;
constexpr std::size_t kDatabaseLockCyclePoolSize = 2U;
constexpr std::size_t kPushOnlyRolePoolSize = 1U;
constexpr std::size_t kOwnerRolePoolSize = 2U;
constexpr std::size_t kTwoPostgresCommandCount = 2U;
constexpr int kDecimalRadix = 10;

[[nodiscard]] std::string BuildLockPushConfigRowSql(std::string_view schema) {
  std::string sql = "UPDATE ";
  sql.append(a2a::server::stores::PushTable(schema));
  sql.append(" SET updated_at = updated_at WHERE task_id = $1 AND config_id = $2");
  return sql;
}

[[nodiscard]] a2a::core::Result<void> LockPushConfigRow(PGconn* connection, std::string_view schema,
                                                        std::string_view task_id, std::string_view config_id) {
  const std::string sql = BuildLockPushConfigRowSql(schema);
  const std::string task_id_value(task_id);
  const std::string config_id_value(config_id);
  const std::array<const char*, 2> values = {task_id_value.c_str(), config_id_value.c_str()};
  a2a::server::stores::PgResult result(PQexecParams(connection, sql.c_str(), static_cast<int>(values.size()), nullptr,
                                                    values.data(), nullptr, nullptr, 0));
  return a2a::server::stores::CheckCommand(connection, result.get(), kLockConcurrentPushRowOperation);
}

[[nodiscard]] a2a::core::Result<void> ConfigurePushPoolStatementTimeout(
    a2a::server::stores::PostgresPushNotificationStore& push_store, std::size_t pool_size) {
  std::vector<a2a::server::stores::PostgresConnectionPool::Lease> leases;
  leases.reserve(pool_size);
  const std::string timeout_sql(kConcurrentStatementTimeoutSql);
  for (std::size_t index = 0; index < pool_size; ++index) {
    auto lease = push_store.AcquireConnectionForTesting();
    if (!lease.ok()) {
      return lease.error();
    }
    const auto configured =
        a2a::server::stores::Exec(lease.value().get(), timeout_sql, kSetConcurrentStatementTimeoutOperation);
    if (!configured.ok()) {
      return configured.error();
    }
    leases.push_back(std::move(lease.value()));
  }
  return {};
}

[[nodiscard]] a2a::core::Result<std::size_t> CountWaitingPostgresLocks(PGconn* connection) {
  const std::string application_name(kIdentityApplicationName);
  const std::array<const char*, 1> values = {application_name.c_str()};
  a2a::server::stores::PgResult result(PQexecParams(connection, kCountWaitingPostgresLocksSql.data(),
                                                    static_cast<int>(values.size()), nullptr, values.data(), nullptr,
                                                    nullptr, 0));
  const auto checked = a2a::server::stores::CheckTuples(connection, result.get(), kCountWaitingPostgresLocksOperation);
  if (!checked.ok()) {
    return checked.error();
  }
  if (PQntuples(result.get()) != 1) {
    return a2a::core::Error::Internal(std::string(kCountWaitingPostgresLocksMissingRowMessage));
  }
  return static_cast<std::size_t>(std::strtoull(PQgetvalue(result.get(), 0, 0), nullptr, kDecimalRadix));
}

[[nodiscard]] a2a::core::Result<void> WaitForPostgresLockWaiters(PGconn* connection, std::size_t expected_count) {
  for (std::size_t attempt = 0; attempt < kLockWaitPollAttempts; ++attempt) {
    const auto count = CountWaitingPostgresLocks(connection);
    if (!count.ok()) {
      return count.error();
    }
    if (count.value() >= expected_count) {
      return {};
    }
    std::this_thread::sleep_for(kLockWaitPollInterval);
  }
  return a2a::core::Error::Internal(std::string(kPostgresLockWaitTimeoutMessage));
}

struct PostgresCommandOutcome final {
  a2a::core::Result<void> result;
  std::string sql_state;
};

[[nodiscard]] PostgresCommandOutcome ExecWithSqlState(PGconn* connection, const std::string& sql,
                                                      std::string_view operation) {
  a2a::server::stores::PgResult result(PQexec(connection, sql.c_str()));
  const char* sql_state = result == nullptr ? nullptr : PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
  std::string sql_state_value = sql_state == nullptr ? std::string{} : std::string(sql_state);
  return PostgresCommandOutcome{
      .result = a2a::server::stores::CheckCommand(connection, result.get(), operation),
      .sql_state = std::move(sql_state_value),
  };
}

[[nodiscard]] SplitRoleCreateOutcome RunTaskAwareCreateWithDiagnostics(
    a2a::server::stores::PostgresPushNotificationStore& push_store,
    const lf::a2a::v1::TaskPushNotificationConfig& config, const a2a::server::TaskStore& task_store) {
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  auto result = push_store.CreateOrUpdateForTask(config, task_store);
  return SplitRoleCreateOutcome{
      .result = std::move(result),
      .diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting(),
  };
}

void ExpectSingleTaskAwarePushCommandDiagnostics(const a2a::server::stores::PostgresOperationDiagnostics& diagnostics) {
  EXPECT_EQ(
      diagnostics
          .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kConnectionAcquireWait)],
      kSinglePostgresCommandCount);
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            kNoPostgresCommandCount);
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      kSinglePostgresCommandCount);
}

void ExpectNoExplicitPushTransactionDiagnostics(const a2a::server::stores::PostgresOperationDiagnostics& diagnostics) {
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTransactionBegin)],
      kNoPostgresCommandCount);
  EXPECT_EQ(diagnostics
                .call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTransactionCommit)],
            kNoPostgresCommandCount);
}

void ExpectSingleTaskAwarePushUpsertDiagnostics(const a2a::server::stores::PostgresOperationDiagnostics& diagnostics) {
  ExpectSingleTaskAwarePushCommandDiagnostics(diagnostics);
  ExpectNoExplicitPushTransactionDiagnostics(diagnostics);
}

struct CreateFirstDeleteOutcome final {
  SplitRoleCreateOutcome create;
  a2a::core::Result<void> deletion;
  bool task_remains;
  bool config_remains;
};

[[nodiscard]] a2a::core::Result<CreateFirstDeleteOutcome> RunCreateFirstDeleteScenario(
    a2a::server::stores::PostgresPushNotificationStore& push_store,
    const a2a::server::stores::PostgresTaskStore& task_store, const a2a::server::stores::PostgresStoreOptions& options,
    const lf::a2a::v1::TaskPushNotificationConfig& updated_config) {
  std::future<SplitRoleCreateOutcome> create;
  std::future<a2a::core::Result<void>> deletion;
  auto blocker = push_store.AcquireConnectionForTesting();
  if (!blocker.ok()) {
    return blocker.error();
  }
  a2a::server::stores::Transaction blocker_transaction(blocker.value().get());
  const auto begun = blocker_transaction.Begin();
  if (!begun.ok()) {
    return begun.error();
  }
  const auto locked =
      LockPushConfigRow(blocker.value().get(), options.schema, updated_config.task_id(), updated_config.id());
  if (!locked.ok()) {
    return locked.error();
  }

  std::barrier create_started(2);
  create = std::async(std::launch::async, [&] {
    create_started.arrive_and_wait();
    return RunTaskAwareCreateWithDiagnostics(push_store, updated_config, task_store);
  });
  create_started.arrive_and_wait();
  const auto create_waiting = WaitForPostgresLockWaiters(blocker.value().get(), kSinglePostgresCommandCount);
  if (!create_waiting.ok()) {
    return create_waiting.error();
  }

  std::barrier delete_started(2);
  deletion = std::async(std::launch::async, [&] {
    delete_started.arrive_and_wait();
    return DeletePostgresTask(push_store, options, updated_config.task_id());
  });
  delete_started.arrive_and_wait();
  const auto delete_waiting = WaitForPostgresLockWaiters(blocker.value().get(), kTwoPostgresCommandCount);
  if (!delete_waiting.ok()) {
    return delete_waiting.error();
  }

  const auto committed = blocker_transaction.Commit();
  if (!committed.ok()) {
    return committed.error();
  }
  auto create_outcome = create.get();
  auto deletion_outcome = deletion.get();
  return CreateFirstDeleteOutcome{
      .create = std::move(create_outcome),
      .deletion = std::move(deletion_outcome),
      .task_remains = task_store.Get(updated_config.task_id()).ok(),
      .config_remains = push_store.Get(updated_config.task_id(), updated_config.id()).ok(),
  };
}

void ExpectCreateFirstDeleteOutcome(const a2a::core::Result<CreateFirstDeleteOutcome>& outcome) {
  ASSERT_TRUE(outcome.ok());
  ASSERT_TRUE(outcome.value().create.result.ok());
  ASSERT_TRUE(outcome.value().deletion.ok());
  ExpectSingleTaskAwarePushUpsertDiagnostics(outcome.value().create.diagnostics);
  EXPECT_FALSE(outcome.value().task_remains);
  EXPECT_FALSE(outcome.value().config_remains);
}

struct ExternalCreateDeleteOutcome final {
  SplitRoleCreateOutcome create;
  a2a::core::Result<void> deletion;
  bool task_remains;
  bool config_remains;
  std::string config_url;
};

[[nodiscard]] a2a::core::Result<ExternalCreateDeleteOutcome> RunExternalCreateDeleteScenario(
    a2a::server::stores::PostgresPushNotificationStore& push_store,
    const a2a::server::stores::PostgresTaskStore& local_task_store,
    const a2a::server::InMemoryTaskStore& external_task_store, const a2a::server::stores::PostgresStoreOptions& options,
    const lf::a2a::v1::TaskPushNotificationConfig& external_config) {
  std::future<SplitRoleCreateOutcome> create;
  std::future<a2a::core::Result<void>> deletion;
  auto blocker = push_store.AcquireConnectionForTesting();
  if (!blocker.ok()) {
    return blocker.error();
  }
  a2a::server::stores::Transaction blocker_transaction(blocker.value().get());
  const auto begun = blocker_transaction.Begin();
  if (!begun.ok()) {
    return begun.error();
  }
  const auto locked =
      LockPushConfigRow(blocker.value().get(), options.schema, external_config.task_id(), external_config.id());
  if (!locked.ok()) {
    return locked.error();
  }

  std::barrier create_started(2);
  create = std::async(std::launch::async, [&] {
    create_started.arrive_and_wait();
    return RunTaskAwareCreateWithDiagnostics(push_store, external_config, external_task_store);
  });
  create_started.arrive_and_wait();
  const auto create_waiting = WaitForPostgresLockWaiters(blocker.value().get(), kSinglePostgresCommandCount);
  if (!create_waiting.ok()) {
    return create_waiting.error();
  }

  std::barrier delete_started(2);
  deletion = std::async(std::launch::async, [&] {
    delete_started.arrive_and_wait();
    return DeletePostgresTask(push_store, options, external_config.task_id());
  });
  delete_started.arrive_and_wait();
  const auto delete_waiting = WaitForPostgresLockWaiters(blocker.value().get(), kTwoPostgresCommandCount);
  if (!delete_waiting.ok()) {
    return delete_waiting.error();
  }

  const auto committed = blocker_transaction.Commit();
  if (!committed.ok()) {
    return committed.error();
  }
  auto create_outcome = create.get();
  auto deletion_outcome = deletion.get();
  const auto stored = push_store.Get(external_config.task_id(), external_config.id());
  return ExternalCreateDeleteOutcome{
      .create = std::move(create_outcome),
      .deletion = std::move(deletion_outcome),
      .task_remains = local_task_store.Get(external_config.task_id()).ok(),
      .config_remains = stored.ok(),
      .config_url = stored.ok() ? stored.value().url() : std::string{},
  };
}

void ExpectExternalCreateDeleteState(const ExternalCreateDeleteOutcome& outcome) {
  EXPECT_FALSE(outcome.task_remains);
  EXPECT_TRUE(outcome.config_remains);
  EXPECT_EQ(outcome.config_url, kConcurrentExternalUpdateUrl);
}

void ExpectExternalCreateDeleteOutcome(const a2a::core::Result<ExternalCreateDeleteOutcome>& outcome) {
  ASSERT_TRUE(outcome.ok());
  ASSERT_TRUE(outcome.value().create.result.ok());
  ASSERT_TRUE(outcome.value().deletion.ok());
  ExpectSingleTaskAwarePushUpsertDiagnostics(outcome.value().create.diagnostics);
  ExpectExternalCreateDeleteState(outcome.value());
}

struct ConflictingWriterOutcome final {
  SplitRoleCreateOutcome first;
  SplitRoleCreateOutcome second;
  std::string final_url;
};

[[nodiscard]] a2a::core::Result<ConflictingWriterOutcome> RunConflictingWriterScenario(
    a2a::server::stores::PostgresPushNotificationStore& push_store,
    const a2a::server::stores::PostgresTaskStore& task_store, const a2a::server::stores::PostgresStoreOptions& options,
    const lf::a2a::v1::TaskPushNotificationConfig& first_config,
    const lf::a2a::v1::TaskPushNotificationConfig& second_config) {
  std::future<SplitRoleCreateOutcome> first;
  std::future<SplitRoleCreateOutcome> second;
  auto blocker = push_store.AcquireConnectionForTesting();
  if (!blocker.ok()) {
    return blocker.error();
  }
  a2a::server::stores::Transaction blocker_transaction(blocker.value().get());
  const auto begun = blocker_transaction.Begin();
  if (!begun.ok()) {
    return begun.error();
  }
  const auto locked =
      LockPushConfigRow(blocker.value().get(), options.schema, first_config.task_id(), first_config.id());
  if (!locked.ok()) {
    return locked.error();
  }

  std::barrier first_started(2);
  first = std::async(std::launch::async, [&] {
    first_started.arrive_and_wait();
    return RunTaskAwareCreateWithDiagnostics(push_store, first_config, task_store);
  });
  first_started.arrive_and_wait();
  const auto first_waiting = WaitForPostgresLockWaiters(blocker.value().get(), kSinglePostgresCommandCount);
  if (!first_waiting.ok()) {
    return first_waiting.error();
  }

  std::barrier second_started(2);
  second = std::async(std::launch::async, [&] {
    second_started.arrive_and_wait();
    return RunTaskAwareCreateWithDiagnostics(push_store, second_config, task_store);
  });
  second_started.arrive_and_wait();
  const auto second_waiting = WaitForPostgresLockWaiters(blocker.value().get(), kTwoPostgresCommandCount);
  if (!second_waiting.ok()) {
    return second_waiting.error();
  }

  const auto committed = blocker_transaction.Commit();
  if (!committed.ok()) {
    return committed.error();
  }
  auto first_outcome = first.get();
  auto second_outcome = second.get();
  const auto stored = push_store.Get(first_config.task_id(), first_config.id());
  if (!stored.ok()) {
    return stored.error();
  }
  return ConflictingWriterOutcome{
      .first = std::move(first_outcome),
      .second = std::move(second_outcome),
      .final_url = stored.value().url(),
  };
}

void ExpectConflictingWriterOutcome(const a2a::core::Result<ConflictingWriterOutcome>& outcome) {
  ASSERT_TRUE(outcome.ok());
  ASSERT_TRUE(outcome.value().first.result.ok());
  ASSERT_TRUE(outcome.value().second.result.ok());
  ExpectSingleTaskAwarePushUpsertDiagnostics(outcome.value().first.diagnostics);
  ExpectSingleTaskAwarePushUpsertDiagnostics(outcome.value().second.diagnostics);
  EXPECT_TRUE(outcome.value().final_url == kConcurrentFirstUpdateUrl ||
              outcome.value().final_url == kConcurrentSecondUpdateUrl);
}

struct DatabaseLockCycleOutcome final {
  SplitRoleCreateOutcome create;
  PostgresCommandOutcome deletion;
  bool task_remains;
  bool config_remains;
  std::string config_url;
};

[[nodiscard]] a2a::core::Result<DatabaseLockCycleOutcome> RunDatabaseLockCycleScenario(
    a2a::server::stores::PostgresPushNotificationStore& push_store,
    const a2a::server::stores::PostgresTaskStore& task_store, const a2a::server::stores::PostgresStoreOptions& options,
    const lf::a2a::v1::TaskPushNotificationConfig& updated_config) {
  std::future<SplitRoleCreateOutcome> create;
  PostgresCommandOutcome deletion;
  {
    auto writer = push_store.AcquireConnectionForTesting();
    if (!writer.ok()) {
      return writer.error();
    }
    a2a::server::stores::Transaction writer_transaction(writer.value().get());
    const auto begun = writer_transaction.Begin();
    if (!begun.ok()) {
      return begun.error();
    }
    const auto locked =
        LockPushConfigRow(writer.value().get(), options.schema, updated_config.task_id(), updated_config.id());
    if (!locked.ok()) {
      return locked.error();
    }

    std::barrier create_started(2);
    create = std::async(std::launch::async, [&] {
      create_started.arrive_and_wait();
      return RunTaskAwareCreateWithDiagnostics(push_store, updated_config, task_store);
    });
    create_started.arrive_and_wait();
    const auto create_waiting = WaitForPostgresLockWaiters(writer.value().get(), kSinglePostgresCommandCount);
    if (!create_waiting.ok()) {
      return create_waiting.error();
    }

    deletion = ExecWithSqlState(
        writer.value().get(),
        BuildDeleteByTaskIdSql(a2a::server::stores::TaskTable(options.schema), kTaskIdColumn, updated_config.task_id()),
        kConcurrentTaskDeleteOperation);
    if (deletion.result.ok()) {
      const auto committed = writer_transaction.Commit();
      if (!committed.ok()) {
        return committed.error();
      }
    }
  }

  auto create_outcome = create.get();
  const auto stored = push_store.Get(updated_config.task_id(), updated_config.id());
  return DatabaseLockCycleOutcome{
      .create = std::move(create_outcome),
      .deletion = std::move(deletion),
      .task_remains = task_store.Get(updated_config.task_id()).ok(),
      .config_remains = stored.ok(),
      .config_url = stored.ok() ? stored.value().url() : std::string{},
  };
}

void ExpectDeleteChosenAsDeadlockVictim(const DatabaseLockCycleOutcome& outcome) {
  ASSERT_FALSE(outcome.deletion.result.ok());
  ASSERT_TRUE(outcome.create.result.ok());
  EXPECT_TRUE(outcome.task_remains);
  EXPECT_TRUE(outcome.config_remains);
  EXPECT_EQ(outcome.config_url, kConcurrentCycleUpdateUrl);
}

void ExpectCreateChosenAsDeadlockVictim(const DatabaseLockCycleOutcome& outcome) {
  ASSERT_TRUE(outcome.deletion.result.ok());
  ASSERT_FALSE(outcome.create.result.ok());
  EXPECT_NE(outcome.create.result.error().message().find(kPostgresDeadlockMessage), std::string_view::npos);
  EXPECT_FALSE(outcome.task_remains);
  EXPECT_FALSE(outcome.config_remains);
}

void ExpectDatabaseLockCycleOutcome(const a2a::core::Result<DatabaseLockCycleOutcome>& outcome) {
  ASSERT_TRUE(outcome.ok());
  ExpectSingleTaskAwarePushUpsertDiagnostics(outcome.value().create.diagnostics);
  if (outcome.value().deletion.sql_state == kPostgresDeadlockSqlState) {
    ExpectDeleteChosenAsDeadlockVictim(outcome.value());
    return;
  }
  ExpectCreateChosenAsDeadlockVictim(outcome.value());
}

void AddExternalAuthorityTask(a2a::server::InMemoryTaskStore& task_store) {
  ASSERT_TRUE(task_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
                      std::string(kMixedCreateTaskId), std::string(kPushListContextId), lf::a2a::v1::TASK_STATE_WORKING,
                      kOldTargetTaskTimestampSeconds))
                  .ok());
}

void ExpectExternalAuthorityPushOnlyCreateSucceeds(std::string_view role_dsn, std::string_view schema,
                                                   a2a::server::InMemoryTaskStore& external_task_store) {
  const a2a::server::stores::PostgresStoreOptions role_options{.connection_string = std::string(role_dsn),
                                                               .schema = std::string(schema),
                                                               .auto_create_schema = false,
                                                               .connection_pool_size = kPushOnlyRolePoolSize};
  a2a::server::stores::PostgresPushNotificationStore push_only_store(role_options);
  const auto created = push_only_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kMixedCreateTaskId), std::string(kMixedCreateConfigId)),
      external_task_store);
  ASSERT_TRUE(created.ok());
  ExpectPushConfigPresent(push_only_store, kMixedCreateTaskId, kMixedCreateConfigId);
}

void ExpectExternalAuthorityPushOnlyRoleWorks(std::string_view dsn_value) {
  const std::string schema = MakePostgresTestSchema(kPushOnlyExternalSchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{
      .connection_string = std::string(dsn_value), .schema = schema, .connection_pool_size = kOwnerRolePoolSize};
  a2a::server::stores::PostgresTaskStore owner_task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_push_store(owner_options);
  auto connection = owner_push_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  ExpectPostgresExecOk(connection.value().get(), BuildEnableTaskRowLevelSecuritySql(schema),
                       kEnableTaskRowLevelSecurityOperation);
  ScopedPostgresRole role(connection.value().get(), MakePostgresTestRole(kSplitRolePrefix),
                          owner_task_store.execution_identity().storage.database, schema);
  ASSERT_TRUE(role.Create().ok());
  ExpectPostgresExecOk(connection.value().get(), BuildRevokeLocalTaskAwarePrivilegesSql(schema, role.role()),
                       kRevokeLocalTaskAwarePrivilegesOperation);
  const std::string role_dsn = BuildRoleDsn(dsn_value, role.role());
  ASSERT_FALSE(role_dsn.empty());
  a2a::server::InMemoryTaskStore external_task_store;
  AddExternalAuthorityTask(external_task_store);
  ExpectExternalAuthorityPushOnlyCreateSucceeds(role_dsn, schema, external_task_store);
}

void ExpectLocalCreateFirstScenario(std::string_view dsn_value) {
  const std::string schema = MakePostgresTestSchema(kCreateFirstSchemaSuffix);
  const std::string push_dsn = BuildEquivalentKeywordDsn(dsn_value);
  ASSERT_FALSE(push_dsn.empty());
  const a2a::server::stores::PostgresStoreOptions task_options{
      .connection_string = std::string(dsn_value), .schema = schema, .connection_pool_size = kConcurrencyTaskPoolSize};
  const a2a::server::stores::PostgresStoreOptions push_options{
      .connection_string = push_dsn, .schema = schema, .connection_pool_size = kCreateFirstPoolSize};
  a2a::server::stores::PostgresTaskStore task_store(task_options);
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  ASSERT_TRUE(ConfigurePushPoolStatementTimeout(push_store, kCreateFirstPoolSize).ok());
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  auto config =
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId));
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, task_store).ok());
  config.set_url(std::string(kConcurrentFirstUpdateUrl));
  ExpectCreateFirstDeleteOutcome(RunCreateFirstDeleteScenario(push_store, task_store, push_options, config));
}

void ExpectConcurrentExternalAuthorityScenario(std::string_view dsn_value) {
  const std::string schema = MakePostgresTestSchema(kExternalConcurrentSchemaSuffix);
  const std::string push_dsn = BuildEquivalentKeywordDsn(dsn_value);
  ASSERT_FALSE(push_dsn.empty());
  const a2a::server::stores::PostgresStoreOptions task_options{
      .connection_string = std::string(dsn_value), .schema = schema, .connection_pool_size = kConcurrencyTaskPoolSize};
  const a2a::server::stores::PostgresStoreOptions push_options{
      .connection_string = push_dsn, .schema = schema, .connection_pool_size = kExternalConcurrentPoolSize};
  a2a::server::stores::PostgresTaskStore local_task_store(task_options);
  a2a::server::InMemoryTaskStore external_task_store;
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  ASSERT_TRUE(ConfigurePushPoolStatementTimeout(push_store, kExternalConcurrentPoolSize).ok());
  AddPostgresTask(local_task_store, kProvenanceTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(external_task_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeTask(
                      std::string(kProvenanceTaskId), std::string(kPushListContextId), lf::a2a::v1::TASK_STATE_WORKING,
                      kOldTargetTaskTimestampSeconds))
                  .ok());
  auto config = a2a::tests::store_conformance::MakeConfig(std::string(kProvenanceTaskId),
                                                          std::string(kTransitionProvenanceConfigId));
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, local_task_store).ok());
  config.set_url(std::string(kConcurrentExternalUpdateUrl));
  ExpectExternalCreateDeleteOutcome(
      RunExternalCreateDeleteScenario(push_store, local_task_store, external_task_store, push_options, config));
}

void ExpectConflictingWriterScenario(std::string_view dsn_value) {
  const std::string schema = MakePostgresTestSchema(kConflictingWriterSchemaSuffix);
  const std::string push_dsn = BuildEquivalentKeywordDsn(dsn_value);
  ASSERT_FALSE(push_dsn.empty());
  const a2a::server::stores::PostgresStoreOptions task_options{
      .connection_string = std::string(dsn_value), .schema = schema, .connection_pool_size = kConcurrencyTaskPoolSize};
  const a2a::server::stores::PostgresStoreOptions push_options{
      .connection_string = push_dsn, .schema = schema, .connection_pool_size = kConflictingWriterPoolSize};
  a2a::server::stores::PostgresTaskStore task_store(task_options);
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  ASSERT_TRUE(ConfigurePushPoolStatementTimeout(push_store, kConflictingWriterPoolSize).ok());
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  const auto base_config =
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId));
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(base_config, task_store).ok());
  auto first_config = base_config;
  first_config.set_url(std::string(kConcurrentFirstUpdateUrl));
  auto second_config = base_config;
  second_config.set_url(std::string(kConcurrentSecondUpdateUrl));
  ExpectConflictingWriterOutcome(
      RunConflictingWriterScenario(push_store, task_store, push_options, first_config, second_config));
}

void ExpectDatabaseLockCycleScenario(std::string_view dsn_value) {
  const std::string schema = MakePostgresTestSchema(kDatabaseLockCycleSchemaSuffix);
  const std::string push_dsn = BuildEquivalentKeywordDsn(dsn_value);
  ASSERT_FALSE(push_dsn.empty());
  const a2a::server::stores::PostgresStoreOptions task_options{
      .connection_string = std::string(dsn_value), .schema = schema, .connection_pool_size = kConcurrencyTaskPoolSize};
  const a2a::server::stores::PostgresStoreOptions push_options{
      .connection_string = push_dsn, .schema = schema, .connection_pool_size = kDatabaseLockCyclePoolSize};
  a2a::server::stores::PostgresTaskStore task_store(task_options);
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  ASSERT_TRUE(ConfigurePushPoolStatementTimeout(push_store, kDatabaseLockCyclePoolSize).ok());
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  auto config =
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId));
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, task_store).ok());
  config.set_url(std::string(kConcurrentCycleUpdateUrl));
  ExpectDatabaseLockCycleOutcome(RunDatabaseLockCycleScenario(push_store, task_store, push_options, config));
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

TEST(StoreConformanceTest, PostgresStorageIdentityUsesResolvedConnectionTarget) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  a2a::server::stores::PostgresConnectionPool pool(dsn_value, 1U);
  auto connection = pool.Acquire();
  ASSERT_TRUE(connection.ok());
  Conninfo options(PQconninfo(connection.value().get()));
  ASSERT_NE(options, nullptr);
  const auto identity = pool.StorageIdentity(std::string(kIdentitySchema));
  const a2a::server::stores::PostgresStorageIdentity expected{
      .host = LibpqValue(PQhost(connection.value().get())),
      .host_address = LibpqValue(PQhostaddr(connection.value().get())),
      .port = LibpqValue(PQport(connection.value().get())),
      .database = LibpqValue(PQdb(connection.value().get())),
      .target_session_attributes = ConninfoValue(options.get(), kConninfoTargetSessionAttributes),
      .schema = std::string(kIdentitySchema),
  };

  EXPECT_EQ(identity, expected);
}

TEST(StoreConformanceTest, PostgresConnectionPoolRejectsMixedConnectionIdentities) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  a2a::server::stores::PostgresConnectionPool baseline(dsn_value, 1U);
  auto mismatched_identity = baseline.ExecutionIdentity(std::string{});
  mismatched_identity.storage.host = std::string(kMismatchedPoolEndpoint);
  a2a::server::stores::OverrideNextPostgresConnectionIdentityForTesting(std::move(mismatched_identity));

  bool rejected = false;
  try {
    (void)a2a::server::stores::PostgresConnectionPool(dsn_value, kIdentityValidationPoolSize);
  } catch (const std::runtime_error& error) {
    rejected = true;
    EXPECT_EQ(std::string_view(error.what()), a2a::server::stores::kPostgresConnectionPoolIdentityMismatchMessage);
  }
  a2a::server::stores::ClearPostgresConnectionIdentityOverrideForTesting();

  EXPECT_TRUE(rejected);
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
  const char* environment_port = std::getenv(kPgPortEnvironmentVariable.data());
  if (environment_port != nullptr && std::string_view(environment_port) != kDefaultPostgresPort) {
    GTEST_SKIP() << kNonDefaultPgPortSkipMessage;
  }
  a2a::server::stores::PostgresConnectionPool default_port(BuildEquivalentKeywordDsn(dsn_value, false), 1U);
  EXPECT_EQ(explicit_port.StorageIdentity(std::string(kDefaultPortIdentitySchema)),
            default_port.StorageIdentity(std::string(kDefaultPortIdentitySchema)));
}

TEST(StoreConformanceTest, PostgresStorageAuthorityClassifiesIdentityDifferencesConservatively) {
  using a2a::server::stores::ClassifyPostgresStorageAuthority;
  using a2a::server::stores::PostgresStorageAuthority;
  a2a::server::stores::PostgresStorageIdentity identity{
      .host = std::string(kIdentityHost),
      .host_address = std::string(kIdentityHostAddress),
      .port = std::string(kIdentityPort),
      .database = std::string(kIdentityDatabase),
      .target_session_attributes = std::string(kIdentityTargetSessionAttributes),
      .schema = std::string(kIdentitySchemaName)};
  EXPECT_EQ(ClassifyPostgresStorageAuthority(identity, identity), PostgresStorageAuthority::kLocal);

  auto different = identity;
  different.host = kOtherIdentityHost;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(identity, different), PostgresStorageAuthority::kUncertain);
  different = identity;
  different.host_address = kOtherIdentityHostAddress;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(identity, different), PostgresStorageAuthority::kUncertain);
  different = identity;
  different.port = kOtherIdentityPort;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(identity, different), PostgresStorageAuthority::kUncertain);
  different = identity;
  different.target_session_attributes = kOtherIdentityTargetSessionAttributes;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(identity, different), PostgresStorageAuthority::kUncertain);
  different = identity;
  different.database = kOtherIdentityDatabase;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(identity, different), PostgresStorageAuthority::kExternal);
  different = identity;
  different.schema = kOtherIdentitySchemaName;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(identity, different), PostgresStorageAuthority::kExternal);

  auto multi_host = identity;
  multi_host.host = kIdentityMultiHost;
  multi_host.port = kIdentityMultiPort;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(multi_host, multi_host), PostgresStorageAuthority::kLocal);
  different = multi_host;
  different.host = kReorderedIdentityMultiHost;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(multi_host, different), PostgresStorageAuthority::kUncertain);

  auto explicit_authority = identity;
  explicit_authority.storage_authority_id = kIdentityStorageAuthorityId;
  different = explicit_authority;
  different.host = kOtherIdentityHost;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(explicit_authority, different), PostgresStorageAuthority::kLocal);
  different.storage_authority_id = kOtherIdentityStorageAuthorityId;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(explicit_authority, different), PostgresStorageAuthority::kExternal);
  different = explicit_authority;
  different.storage_authority_id.clear();
  EXPECT_EQ(ClassifyPostgresStorageAuthority(explicit_authority, different), PostgresStorageAuthority::kUncertain);
  different = explicit_authority;
  different.database = kOtherIdentityDatabase;
  EXPECT_EQ(ClassifyPostgresStorageAuthority(explicit_authority, different), PostgresStorageAuthority::kExternal);
}

TEST(StoreConformanceTest, ExplicitPostgresStorageAuthorityIdCanProveExternalAuthority) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  const std::string schema = MakePostgresTestSchema(kExplicitExternalAuthoritySchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions task_options{
      .connection_string = dsn_value,
      .schema = schema,
      .storage_authority_id = std::string(kIdentityStorageAuthorityId)};
  const a2a::server::stores::PostgresStoreOptions push_options{
      .connection_string = dsn_value,
      .schema = schema,
      .storage_authority_id = std::string(kOtherIdentityStorageAuthorityId)};
  a2a::server::stores::PostgresTaskStore task_store(task_options);
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  AddPostgresTask(task_store, kAuthorityTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();

  const auto created = push_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kAuthorityTaskId), std::string(kAuthorityConfigId)),
      task_store);

  ASSERT_TRUE(created.ok());
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            kSinglePostgresCommandCount);
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      kSinglePostgresCommandCount);
  ASSERT_TRUE(DeletePostgresTask(push_store, task_options, kAuthorityTaskId).ok());
  ExpectPushConfigPresent(push_store, kAuthorityTaskId, kAuthorityConfigId);
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

constexpr std::string_view kSharedPoolSchemaSuffix = "shared_pool";
constexpr std::size_t kConfiguredBundlePoolSize = 2U;

void ExpectSharedPostgresBundlePool(a2a::server::stores::PostgresTaskStore& task_store,
                                    a2a::server::stores::PostgresPushNotificationStore& push_store) {
  ASSERT_EQ(task_store.connection_pool_for_testing(), push_store.connection_pool_for_testing());
  EXPECT_EQ(task_store.connection_pool_for_testing()->capacity(), kConfiguredBundlePoolSize);
}

void ExpectSharedPostgresBundleListShortcut(a2a::server::stores::PostgresTaskStore& task_store,
                                            a2a::server::stores::PostgresPushNotificationStore& push_store) {
  AddPostgresTask(task_store, kSessionPolicyTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kSessionPolicyTaskId),
                                                                            std::string(kSessionPolicyConfigId)))
                  .ok());
  ResetPushConfigListDiagnostics();
  ASSERT_TRUE(push_store.ListForTask(kSessionPolicyTaskId, kUnboundedPushListPageSize, {}, task_store).ok());
  ExpectSinglePushConfigListCommand();
}

void ExpectConfiguredPostgresBundle(std::string_view dsn) {
  const a2a::server::stores::PostgresStoreFactory factory({.connection_string = std::string(dsn),
                                                           .schema = MakePostgresTestSchema(kSharedPoolSchemaSuffix),
                                                           .connection_pool_size = kConfiguredBundlePoolSize});
  auto bundle = factory.CreateStoreBundle();
  ASSERT_TRUE(bundle.ok());

  auto* task_store = dynamic_cast<a2a::server::stores::PostgresTaskStore*>(bundle.value().task_store.get());
  auto* push_store = dynamic_cast<a2a::server::stores::PostgresPushNotificationStore*>(bundle.value().push_store.get());
  ASSERT_NE(task_store, nullptr);
  ASSERT_NE(push_store, nullptr);
  ExpectSharedPostgresBundlePool(*task_store, *push_store);
  ExpectSharedPostgresBundleListShortcut(*task_store, *push_store);
}

TEST(StoreConformanceTest, PostgresBundleSharesConfiguredConnectionPool) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  ExpectConfiguredPostgresBundle(dsn_value);
}

TEST(StoreConformanceTest, SharedPostgresPoolWithDifferentSchemasUsesAuthoritativeTaskLookup) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  const a2a::server::stores::PostgresStoreOptions task_options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kSharedPoolTaskSchemaSuffix)};
  const a2a::server::stores::PostgresStoreOptions push_options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kSharedPoolPushSchemaSuffix)};
  auto pool = std::make_shared<a2a::server::stores::PostgresConnectionPool>(dsn_value, kOwnerRolePoolSize);
  a2a::server::stores::PostgresTaskStore task_store(pool, task_options);
  a2a::server::stores::PostgresPushNotificationStore push_store(pool, push_options);
  AddPostgresTask(task_store, kSessionPolicyTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kSessionPolicyTaskId),
                                                                            std::string(kSessionPolicyConfigId)))
                  .ok());

  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto listed = push_store.ListForTask(kSessionPolicyTaskId, kUnboundedPushListPageSize, {}, task_store);
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value().configs_size(), kSessionPolicyExpectedConfigCount);
  ExpectTaskFirstPushConfigListCommands();
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

TEST(StoreConformanceTest, PostgresPushSchemaDropsPushSecondaryIndexes) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema("push_index_amplification");
  a2a::server::stores::PostgresPushNotificationStore store(
      a2a::server::stores::PostgresStoreOptions{.connection_string = dsn_value, .schema = schema});
  auto connection = store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());

  const std::string legacy_index =
      a2a::server::stores::QualifiedSqlIdentifier(schema, a2a::server::stores::kPushConfigsTaskIndex);
  const std::string ordering_index =
      a2a::server::stores::QualifiedSqlIdentifier(schema, a2a::server::stores::kPushConfigsCreatedSequenceIndex);
  const std::array<const char*, 2> values = {legacy_index.c_str(), ordering_index.c_str()};
  a2a::server::stores::PgResult result(PQexecParams(connection.value().get(), kIndexPresenceSql.data(),
                                                    static_cast<int>(values.size()), nullptr, values.data(), nullptr,
                                                    nullptr, 0));
  ASSERT_TRUE(
      a2a::server::stores::CheckTuples(connection.value().get(), result.get(), kInspectPushIndexesOperation).ok());
  ASSERT_EQ(PQntuples(result.get()), 1);
  EXPECT_NE(PQgetisnull(result.get(), 0, 0), 0);
  EXPECT_NE(PQgetisnull(result.get(), 0, 1), 0);
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

TEST(StoreConformanceTest, PostgresTaskAwarePushListPreservesTaskFirstValidationOrder) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kListValidationOrderSchemaSuffix)};
  a2a::server::stores::PostgresTaskStore task_store(options);
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  ExpectMissingTaskPrecedesTaskAwarePushListValidation(push_store, task_store);

  AddPostgresTask(task_store, kMissingTaskAwareListTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  const auto malformed_page_token = push_store.ListForTask(kMissingTaskAwareListTaskId, kValidTaskAwareListPageSize,
                                                           kMalformedTaskAwareListPageToken, task_store);
  ASSERT_FALSE(malformed_page_token.ok());
  EXPECT_EQ(malformed_page_token.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_FALSE(malformed_page_token.error().protocol_code().has_value());
}

void SeedSessionPolicyConfig(a2a::server::stores::PostgresTaskStore& task_store,
                             a2a::server::stores::PostgresPushNotificationStore& push_store) {
  AddPostgresTask(task_store, kSessionPolicyTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kSessionPolicyTaskId),
                                                                            std::string(kSessionPolicyConfigId)))
                  .ok());
}

void ExpectSeparatePoolSessionPolicyList(const std::string& task_dsn, const std::string& push_dsn,
                                         const std::string& schema) {
  const a2a::server::stores::PostgresStoreOptions task_options{.connection_string = task_dsn,
                                                               .schema = schema,
                                                               .auto_create_schema = false,
                                                               .connection_pool_size = kPushOnlyRolePoolSize};
  const a2a::server::stores::PostgresStoreOptions push_options{.connection_string = push_dsn,
                                                               .schema = schema,
                                                               .auto_create_schema = false,
                                                               .connection_pool_size = kPushOnlyRolePoolSize};
  a2a::server::stores::PostgresTaskStore task_store(task_options);
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  ASSERT_TRUE(task_store.Get(kSessionPolicyTaskId).ok());

  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto listed = push_store.ListForTask(kSessionPolicyTaskId, kUnboundedPushListPageSize, {}, task_store);
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value().configs_size(), kSessionPolicyExpectedConfigCount);
  EXPECT_EQ(listed.value().configs(0).id(), kSessionPolicyConfigId);
  ExpectTaskFirstPushConfigListCommands();
}

void ExpectSeparatePostgresPoolsPreserveTaskSessionPolicyContext(const char* dsn_value) {
  const std::string schema = MakePostgresTestSchema(kSessionPolicySchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{
      .connection_string = dsn_value, .schema = schema, .connection_pool_size = kOwnerRolePoolSize};
  a2a::server::stores::PostgresTaskStore owner_task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_push_store(owner_options);
  SeedSessionPolicyConfig(owner_task_store, owner_push_store);

  auto admin_connection = owner_push_store.AcquireConnectionForTesting();
  ASSERT_TRUE(admin_connection.ok());
  ScopedPostgresRole role(admin_connection.value().get(), MakePostgresTestRole(kSplitRolePrefix),
                          owner_task_store.execution_identity().storage.database, schema);
  ASSERT_TRUE(role.Create().ok());
  ExpectPostgresExecOk(admin_connection.value().get(), BuildTaskSessionPolicySql(schema, role.role()),
                       kInstallSessionPolicyOperation);
  const std::string task_dsn = BuildRoleDsnWithStartupOption(
      dsn_value, role.role(), BuildSessionPolicyStartupOption(kSessionPolicyVisibleTenant));
  const std::string push_dsn = BuildRoleDsnWithStartupOption(
      dsn_value, role.role(), BuildSessionPolicyStartupOption(kSessionPolicyHiddenTenant));
  ASSERT_FALSE(task_dsn.empty());
  ASSERT_FALSE(push_dsn.empty());

  ExpectSeparatePoolSessionPolicyList(task_dsn, push_dsn, schema);
  ExpectPostgresExecOk(admin_connection.value().get(), BuildDropTaskSessionPolicySql(schema),
                       kDropSessionPolicyOperation);
}

TEST(StoreConformanceTest, SeparatePostgresPoolsPreserveTaskSessionPolicyContext) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  ExpectSeparatePostgresPoolsPreserveTaskSessionPolicyContext(dsn_value);
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

  const auto invalid_missing_task = push_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kMissingAtomicCreateTaskId), ""), task_store);
  ExpectPushConfigTaskNotFound(invalid_missing_task);

  const auto invalid_existing_task = push_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), ""), task_store);
  ExpectPushConfigValidationError(invalid_existing_task);

  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto created = push_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId)),
      task_store);
  ASSERT_TRUE(created.ok());
  ExpectSinglePushConfigUpsertWithoutTaskGet();

  const auto missing =
      push_store.CreateOrUpdateForTask(a2a::tests::store_conformance::MakeConfig(
                                           std::string(kMissingAtomicCreateTaskId), std::string(kAtomicCreateConfigId)),
                                       task_store);
  ExpectPushConfigTaskNotFound(missing);
}

TEST(StoreConformanceTest, PostgresPushConfigDirectCreatePreservesExternalOwnership) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{.connection_string = dsn_value,
                                                          .schema = MakePostgresTestSchema("push_direct_external")};
  a2a::server::stores::PostgresTaskStore local_task_store(options);
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  const auto config =
      a2a::tests::store_conformance::MakeConfig(std::string(kMixedCreateTaskId), std::string(kMixedCreateConfigId));

  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  ASSERT_TRUE(push_store.CreateOrUpdate(config).ok());
  ExpectSinglePushConfigUpsertWithoutTaskGet();

  AddPostgresTask(local_task_store, kMixedCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(DeletePostgresTask(push_store, options, kMixedCreateTaskId).ok());
  ExpectPushConfigPresent(push_store, kMixedCreateTaskId, kMixedCreateConfigId);
}

TEST(StoreConformanceTest, PostgresPushConfigDirectGetPreservesMissingCollectionClassification) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kDirectGetClassificationSchemaSuffix)};
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  ASSERT_TRUE(push_store
                  .CreateOrUpdate(a2a::tests::store_conformance::MakeConfig(std::string(kMixedCreateTaskId),
                                                                            std::string(kMixedCreateConfigId)))
                  .ok());

  ExpectDirectPushConfigPresentWithSingleCommand(push_store);
  ExpectDirectPushConfigMissingConfig(push_store);
  ExpectDirectPushConfigMissingTask(push_store);
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
  ExpectSinglePushConfigUpsertWithoutTaskGet();
  config.set_url(std::string(kLargeConfigUpdatedUrl));
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  ASSERT_TRUE(push_store.CreateOrUpdateForTask(config, task_store).ok());
  ExpectSinglePushConfigUpsertWithoutTaskGet();

  const auto stored = push_store.Get(kAtomicCreateTaskId, kAtomicCreateConfigId);
  ExpectStoredLargePushConfig(stored);
}

TEST(StoreConformanceTest, UncertainPostgresAuthorityDoesNotWriteExternalProvenance) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  const std::string schema = MakePostgresTestSchema(kUncertainAuthoritySchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions task_options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresTaskStore task_store(task_options);
  const std::string_view alternate_target_session_attributes =
      task_store.storage_identity().target_session_attributes == kIdentityTargetSessionAttributes
          ? kIdentityPrimaryTargetSessionAttributes
          : kIdentityTargetSessionAttributes;
  const std::string push_dsn = BuildEquivalentKeywordDsn(dsn_value, true, alternate_target_session_attributes);
  ASSERT_FALSE(push_dsn.empty());
  const a2a::server::stores::PostgresStoreOptions push_options{.connection_string = push_dsn, .schema = schema};
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  AddPostgresTask(task_store, kAuthorityTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);

  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto created = push_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kAuthorityTaskId), std::string(kAuthorityConfigId)),
      task_store);
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();

  ExpectUncertainAuthorityCreateRejected(created, diagnostics, push_store);
}

TEST(StoreConformanceTest, DifferentPostgresSchemasAreConfirmedExternalAuthority) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  const a2a::server::stores::PostgresStoreOptions task_options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kExternalTaskAuthoritySchemaSuffix)};
  const a2a::server::stores::PostgresStoreOptions push_options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kExternalPushAuthoritySchemaSuffix)};
  a2a::server::stores::PostgresTaskStore task_store(task_options);
  a2a::server::stores::PostgresPushNotificationStore push_store(push_options);
  AddPostgresTask(task_store, kAuthorityTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);

  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto created = push_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kAuthorityTaskId), std::string(kAuthorityConfigId)),
      task_store);
  const auto diagnostics = a2a::server::stores::TakePostgresOperationDiagnosticsForTesting();

  ASSERT_TRUE(created.ok());
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            kSinglePostgresCommandCount);
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      kSinglePostgresCommandCount);
  ExpectPushConfigPresent(push_store, kAuthorityTaskId, kAuthorityConfigId);
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
                  .CreateOrUpdateForTask(a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId),
                                                                                   std::string(kAtomicCreateConfigId)),
                                         task_store)
                  .ok());

  const auto outcome = RunConcurrentPushConfigDelete(push_store, task_store, options);
  ExpectConcurrentDeleteOutcome(outcome);
}

TEST(StoreConformanceTest, PostgresLocalCreateLockFirstSerializesBeforeTaskDelete) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  ExpectLocalCreateFirstScenario(dsn_value);
}

TEST(StoreConformanceTest, ConcurrentExternalAuthorityCreateSurvivesLocalTaskDelete) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  ExpectConcurrentExternalAuthorityScenario(dsn_value);
}

TEST(StoreConformanceTest, ConcurrentLocalPushConfigWritersSerializeWithoutExtraCommands) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  ExpectConflictingWriterScenario(dsn_value);
}

TEST(StoreConformanceTest, PostgresDetectsLocalCreateDeleteDatabaseLockCycle) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  ExpectDatabaseLockCycleScenario(dsn_value);
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
    ExpectSplitRoleCreateDiagnostics(diagnostics);
    ExpectPushConfigPresent(owner_push_store, kSplitRoleTaskId, kSplitRoleConfigId);
  }
}

TEST(StoreConformanceTest, TaskAwarePushCreateDoesNotRequireTaskUpdatePrivilege) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema(kReadOnlyTaskRoleSchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{
      .connection_string = dsn_value, .schema = schema, .connection_pool_size = 2U};
  a2a::server::stores::PostgresTaskStore owner_task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_push_store(owner_options);
  AddPostgresTask(owner_task_store, kSplitRoleTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);

  auto admin_connection = owner_push_store.AcquireConnectionForTesting();
  ASSERT_TRUE(admin_connection.ok());
  ScopedPostgresRole role(admin_connection.value().get(), MakePostgresTestRole(kSplitRolePrefix),
                          owner_task_store.execution_identity().storage.database, schema);
  ASSERT_TRUE(role.Create().ok());
  ASSERT_TRUE(a2a::server::stores::Exec(admin_connection.value().get(),
                                        BuildGrantTaskSelectSql(role.role(), a2a::server::stores::TaskTable(schema)),
                                        kGrantTaskSelectOperation)
                  .ok());
  const std::string role_dsn = BuildRoleDsn(dsn_value, role.role());
  ASSERT_FALSE(role_dsn.empty());

  {
    const a2a::server::stores::PostgresStoreOptions role_options{
        .connection_string = role_dsn, .schema = schema, .auto_create_schema = false, .connection_pool_size = 2U};
    a2a::server::stores::PostgresTaskStore role_task_store(role_options);
    a2a::server::stores::PostgresPushNotificationStore role_push_store(role_options);

    a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
    const auto created = role_push_store.CreateOrUpdateForTask(
        a2a::tests::store_conformance::MakeConfig(std::string(kSplitRoleTaskId), std::string(kSplitRoleConfigId)),
        role_task_store);

    ASSERT_TRUE(created.ok());
    ExpectSinglePushConfigUpsertWithoutTaskGet();
    ExpectPushConfigPresent(owner_push_store, kSplitRoleTaskId, kSplitRoleConfigId);
  }
}

TEST(StoreConformanceTest, SplitRoleCreatePreservesExternalConfigWhenDeletionAlreadyInProgress) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema("push_split_role_external_race");
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
  RunAndExpectSplitRoleDeletionFirstScenario(role_push_store, task_store, owner_push_store, owner_options);
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

  ExpectLocalTaskStoreProvenanceAfterConflict(push_store, local_task_store, external_task_store, options, config);
  ExpectExternalTaskStoreProvenanceAfterConflict(push_store, local_task_store, external_task_store, options, config);
}

TEST(StoreConformanceTest, PostgresTaskPushConfigLockFunctionIsNotPubliclyExecutable) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kTaskLockPrivilegeSchemaSuffix)};
  a2a::server::stores::PostgresTaskStore task_store(options);
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);

  const auto denied = RunReadOnlyTaskLockAttempt(push_store, options);

  ASSERT_TRUE(denied.ok());
  EXPECT_TRUE(denied.value());
}

TEST(StoreConformanceTest, PostgresTaskDeleteCleanupRunsOnlyAfterDeletion) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const a2a::server::stores::PostgresStoreOptions options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kSuppressedDeleteSchemaSuffix)};
  a2a::server::stores::PostgresTaskStore task_store(options);
  a2a::server::stores::PostgresPushNotificationStore push_store(options);
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  ASSERT_TRUE(push_store
                  .CreateOrUpdateForTask(a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId),
                                                                                   std::string(kAtomicCreateConfigId)),
                                         task_store)
                  .ok());
  auto connection = push_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  ASSERT_TRUE(a2a::server::stores::Exec(connection.value().get(), BuildRetainTaskTriggerSql(options.schema),
                                        kInstallRetainTaskTriggerOperation)
                  .ok());

  ASSERT_TRUE(DeletePostgresTask(push_store, options, kAtomicCreateTaskId).ok());

  EXPECT_TRUE(task_store.Get(kAtomicCreateTaskId).ok());
  ExpectPushConfigPresent(push_store, kAtomicCreateTaskId, kAtomicCreateConfigId);
}

TEST(StoreConformanceTest, AutoCreatedTaskAwarePushSchemaPassesManagedValidation) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  const a2a::server::stores::PostgresStoreOptions options{
      .connection_string = dsn_value, .schema = MakePostgresTestSchema(kManagedValidationSchemaSuffix)};

  a2a::server::stores::PostgresTaskStore task_store(options);
  EXPECT_NO_THROW(static_cast<void>(a2a::server::stores::PostgresPushNotificationStore(options)));
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresCurrentMigration) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema(kExternalMigrationSchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresPushNotificationStore owner_store(owner_options);
  auto connection = owner_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  ASSERT_TRUE(a2a::server::stores::Exec(connection.value().get(), BuildClearPushMigrationSql(schema),
                                        kClearPushMigrationOperation)
                  .ok());
  const a2a::server::stores::PostgresStoreOptions managed_options{
      .connection_string = dsn_value, .schema = schema, .auto_create_schema = false};

  ExpectManagedPushSchemaRejected(managed_options);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresCleanupTrigger) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema(kMissingCleanupTriggerSchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresPushNotificationStore owner_store(owner_options);
  auto connection = owner_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  ASSERT_TRUE(a2a::server::stores::Exec(connection.value().get(), BuildDropPushCleanupTriggerSql(schema),
                                        kDropPushCleanupTriggerOperation)
                  .ok());
  const a2a::server::stores::PostgresStoreOptions managed_options{
      .connection_string = dsn_value, .schema = schema, .auto_create_schema = false};

  ExpectManagedPushSchemaRejected(managed_options);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRejectsLegacyTaskForeignKey) {
  ExpectManagedPushSchemaMutationRejected(kLegacyForeignKeySchemaSuffix, BuildRestoreLegacyTaskForeignKeySql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresCleanupImplementation) {
  ExpectManagedPushSchemaMutationRejected(kCleanupImplementationSchemaSuffix, BuildNoOpCleanupFunctionSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresTaskLockImplementation) {
  ExpectManagedPushSchemaMutationRejected(kLockImplementationSchemaSuffix, BuildNoOpTaskLockFunctionSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaPreservesQuotedIdentifierCase) {
  ExpectManagedPushSchemaMutationRejected(kCleanupIdentifierCaseSchemaSuffix, BuildWrongCaseCleanupFunctionSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresCleanupVersion) {
  ExpectManagedPushSchemaMutationRejected(kCleanupVersionSchemaSuffix, BuildClearCleanupMigrationSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRejectsPublicLockExecute) {
  ExpectManagedPushSchemaMutationRejected(kLockPublicExecuteSchemaSuffix, BuildGrantPublicLockExecuteSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRejectsPublicCleanupExecute) {
  ExpectManagedPushSchemaMutationRejected(kCleanupPublicExecuteSchemaSuffix, BuildGrantPublicCleanupExecuteSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRejectsCleanupTriggerWhenClause) {
  ExpectManagedPushSchemaMutationRejected(kCleanupTriggerWhenSchemaSuffix, BuildCleanupTriggerWithWhenSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresBooleanProvenance) {
  ExpectManagedPushSchemaMutationRejected(kProvenanceTypeSchemaSuffix, BuildWrongProvenanceTypeSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresNotNullProvenance) {
  ExpectManagedPushSchemaMutationRejected(kProvenanceNotNullSchemaSuffix, BuildNullableProvenanceSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresFalseProvenanceDefault) {
  ExpectManagedPushSchemaMutationRejected(kProvenanceDefaultSchemaSuffix, BuildTrueProvenanceDefaultSql);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresTaskLockOwnerPrivileges) {
  ExpectSecurityDefinerOwnerPrivilegesRequired(kLockOwnerPrivilegeSchemaSuffix, SecurityDefinerOwnerCase::kTaskLock);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRequiresCleanupOwnerPrivileges) {
  ExpectSecurityDefinerOwnerPrivilegesRequired(kCleanupOwnerPrivilegeSchemaSuffix, SecurityDefinerOwnerCase::kCleanup);
}

TEST(StoreConformanceTest, ExternallyManagedPushSchemaRejectsCleanupOwnerFilteredByPushRowLevelSecurity) {
  ExpectCleanupOwnerRowLevelSecurityRejected();
}

TEST(StoreConformanceTest, PostgresLocalTaskAwarePushCreateRejectsTaskRowLevelSecurity) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  const std::string schema = MakePostgresTestSchema(kRowLevelSecuritySchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresTaskStore task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_store(owner_options);
  AddPostgresTask(task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  auto connection = owner_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  ASSERT_TRUE(a2a::server::stores::Exec(connection.value().get(), BuildEnableTaskRowLevelSecuritySql(schema),
                                        kEnableTaskRowLevelSecurityOperation)
                  .ok());
  const a2a::server::stores::PostgresStoreOptions managed_options{
      .connection_string = dsn_value, .schema = schema, .auto_create_schema = false};

  a2a::server::stores::PostgresPushNotificationStore managed_store(managed_options);
  const auto created = managed_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId)),
      task_store);
  ASSERT_FALSE(created.ok());
  EXPECT_NE(std::string_view(created.error().message()).find(kExpectedTaskRowLevelSecurityError),
            std::string_view::npos);
}

TEST(StoreConformanceTest, ExternalAuthorityPushOnlyRoleDoesNotRequireTaskPrivilegesOrRejectTaskRls) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << "A2A_TEST_POSTGRES_DSN is not set";
  }
  ExpectExternalAuthorityPushOnlyRoleWorks(dsn_value);
}

void DropTaskAwarePushSchemaObjects(PGconn* connection, std::string_view schema) {
  ASSERT_TRUE(
      a2a::server::stores::Exec(connection, BuildDropTaskAwarePushSchemaSql(schema), kMutateManagedPushSchemaOperation)
          .ok());
}

void ExpectExternalAuthorityPushOnlyCreate(a2a::server::stores::PostgresPushNotificationStore& push_store,
                                           const a2a::server::TaskStore& external_task_store) {
  ASSERT_TRUE(push_store
                  .CreateOrUpdateForTask(a2a::tests::store_conformance::MakeConfig(std::string(kMixedCreateTaskId),
                                                                                   std::string(kMixedCreateConfigId)),
                                         external_task_store)
                  .ok());
  ExpectPushConfigPresent(push_store, kMixedCreateTaskId, kMixedCreateConfigId);
}

void ExpectPushOnlySchemaLocalCreateDiagnostics(const a2a::server::stores::PostgresOperationDiagnostics& diagnostics) {
  EXPECT_EQ(diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kTaskGet)],
            kSinglePostgresCommandCount);
  EXPECT_EQ(
      diagnostics.call_count[static_cast<std::size_t>(a2a::server::stores::PostgresDiagnosticPhase::kPushConfigUpsert)],
      kNoPostgresCommandCount);
}

void ExpectPushOnlySchemaRejectsLocalTaskAwareCreate(a2a::server::stores::PostgresPushNotificationStore& push_store,
                                                     a2a::server::stores::PostgresTaskStore& local_task_store) {
  a2a::server::stores::ResetPostgresOperationDiagnosticsForTesting();
  const auto local_create = push_store.CreateOrUpdateForTask(
      a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId), std::string(kAtomicCreateConfigId)),
      local_task_store);

  ASSERT_FALSE(local_create.ok());
  EXPECT_EQ(local_create.error().code(), a2a::core::ErrorCode::kInternal);
  EXPECT_NE(local_create.error().message().find(a2a::server::stores::kTaskPushConfigMigrationId),
            std::string_view::npos);
  ExpectPushOnlySchemaLocalCreateDiagnostics(a2a::server::stores::TakePostgresOperationDiagnosticsForTesting());
}

void DropTaskTableForPushOnlySchema(PGconn* connection, std::string_view schema) {
  ASSERT_TRUE(a2a::server::stores::Exec(connection, BuildDropTaskTableSql(schema), kDropTaskTableOperation).ok());
}

void ExpectPushOnlySchemaWithoutTaskAwareObjects(const char* dsn_value) {
  const std::string schema = MakePostgresTestSchema(kPushOnlyNoTaskAwareSchemaSuffix);
  const a2a::server::stores::PostgresStoreOptions owner_options{.connection_string = dsn_value, .schema = schema};
  a2a::server::stores::PostgresTaskStore local_task_store(owner_options);
  a2a::server::stores::PostgresPushNotificationStore owner_push_store(owner_options);
  AddPostgresTask(local_task_store, kAtomicCreateTaskId, kPushListContextId, lf::a2a::v1::TASK_STATE_WORKING,
                  kOldTargetTaskTimestampSeconds);
  auto connection = owner_push_store.AcquireConnectionForTesting();
  ASSERT_TRUE(connection.ok());
  DropTaskAwarePushSchemaObjects(connection.value().get(), schema);

  const a2a::server::stores::PostgresStoreOptions managed_options{
      .connection_string = dsn_value, .schema = schema, .auto_create_schema = false};
  a2a::server::stores::PostgresPushNotificationStore push_only_store(managed_options);
  a2a::server::InMemoryTaskStore external_task_store;
  AddExternalAuthorityTask(external_task_store);
  ExpectExternalAuthorityPushOnlyCreate(push_only_store, external_task_store);
  ExpectPushOnlySchemaRejectsLocalTaskAwareCreate(push_only_store, local_task_store);

  DropTaskTableForPushOnlySchema(connection.value().get(), schema);
  a2a::server::stores::PostgresPushNotificationStore pure_push_only_store(managed_options);
  ExpectExternalAuthorityPushOnlyCreate(pure_push_only_store, external_task_store);
}

TEST(StoreConformanceTest, ExternallyManagedPushOnlySchemaMayOmitTaskAwareObjects) {
  const char* dsn_value = GetPostgresDsn();
  if (dsn_value == nullptr || std::string_view(dsn_value).empty()) {
    GTEST_SKIP() << kPostgresDsnMissingSkipMessage;
  }
  ExpectPushOnlySchemaWithoutTaskAwareObjects(dsn_value);
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
                  .CreateOrUpdateForTask(a2a::tests::store_conformance::MakeConfig(std::string(kAtomicCreateTaskId),
                                                                                   std::string(kAtomicCreateConfigId)),
                                         task_store)
                  .ok());
  const auto outcome = RunLeastPrivilegeTaskDelete(push_store, options);
  ExpectLeastPrivilegeDeleteOutcome(outcome);
}
#endif

}  // namespace
