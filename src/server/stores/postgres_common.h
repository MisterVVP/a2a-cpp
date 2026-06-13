// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <libpq-fe.h>

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/server/stores/store_factory.h"

namespace a2a::server::stores {

inline constexpr std::size_t kDefaultPostgresConnectionPoolSize = 4;

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

  class Lease final {
   public:
    Lease(PostgresConnectionPool* pool, PgConnection connection);
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    Lease(Lease&& other) noexcept;
    Lease& operator=(Lease&& other) noexcept = delete;
    ~Lease();

    [[nodiscard]] PGconn* get() const noexcept;

   private:
    PostgresConnectionPool* pool_ = nullptr;
    PgConnection connection_;
  };

  [[nodiscard]] core::Result<Lease> Acquire();

 private:
  [[nodiscard]] core::Result<PgConnection> OpenConnection() const;
  void Return(PgConnection connection);

  std::string connection_string_;
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
