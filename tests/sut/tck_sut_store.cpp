// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/tck_sut_store.h"

#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "sut/tck_sut.h"

namespace a2a::tests::sut {
namespace {

constexpr std::string_view kMissingPostgresDsnMessage =
    "A2A_TCK_POSTGRES_DSN must be set when A2A_TCK_STORE_BACKEND=postgres";
constexpr std::string_view kUnsupportedStoreBackendMessage = "Unsupported A2A_TCK_STORE_BACKEND: ";
constexpr std::string_view kInvalidPostgresPoolSizeMessage = "A2A_TCK_POSTGRES_POOL_SIZE must be a positive integer";

[[nodiscard]] std::string_view GetEnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string_view{} : std::string_view{value};
}

[[nodiscard]] core::Result<std::size_t> GetPostgresPoolSize() {
  const std::string_view value = GetEnvironmentValue(kPostgresPoolSizeEnv);
  if (value.empty()) {
    return server::stores::kDefaultPostgresConnectionPoolSize;
  }
  std::size_t size = 0U;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), size);
  if (error != std::errc{} || end != value.data() + value.size() || size == 0U) {
    return core::Error::Validation(std::string{kInvalidPostgresPoolSizeMessage});
  }
  return size;
}

}  // namespace

core::Result<server::stores::StoreBundle> CreateStoreBundleFromEnvironment() {
  const std::string_view backend = GetEnvironmentValue(kStoreBackendEnv);
  if (backend.empty() || backend == kInMemoryBackend) {
    const server::stores::InMemoryStoreFactory factory;
    return factory.CreateStoreBundle();
  }
  if (backend != kPostgresBackend) {
    std::string message;
    message.reserve(kUnsupportedStoreBackendMessage.size() + backend.size());
    message.append(kUnsupportedStoreBackendMessage);
    message.append(backend);
    return core::Error::Validation(std::move(message));
  }
  const std::string_view dsn = GetEnvironmentValue(kPostgresDsnEnv);
  if (dsn.empty()) {
    return core::Error::Validation(std::string{kMissingPostgresDsnMessage});
  }
  const std::string_view schema = GetEnvironmentValue(kPostgresSchemaEnv);
  const auto pool_size = GetPostgresPoolSize();
  if (!pool_size.ok()) {
    return pool_size.error();
  }
  server::stores::PostgresStoreOptions options{.connection_string = std::string{dsn},
                                               .schema = std::string{schema.empty() ? kDefaultPostgresSchema : schema},
                                               .auto_create_schema = true,
                                               .connection_pool_size = pool_size.value()};
  const server::stores::PostgresStoreFactory factory(std::move(options));
  return factory.CreateStoreBundle();
}

}  // namespace a2a::tests::sut
