// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

namespace a2a::tests::sut {

constexpr std::string_view kDefaultHost = "127.0.0.1";
constexpr int kDefaultPort = 50061;
constexpr int kGrpcPortOffset = 1;
constexpr std::string_view kRestApiBasePath = "/a2a";
constexpr std::string_view kJsonRpcPath = "/rpc";
constexpr std::string_view kRequiredExtensionUri = "urn:a2a:tck:required-extension";
constexpr std::string_view kPostgresBackend = "postgres";
constexpr std::string_view kInMemoryBackend = "inmemory";
constexpr std::string_view kDefaultPostgresSchema = "public";
constexpr std::string_view kExtendedCardModeConfigured = "configured";
constexpr std::string_view kExtendedCardModeDeclaredOnly = "declared_only";
constexpr std::string_view kExtendedCardModeDisabled = "disabled";
constexpr char kStoreBackendEnv[] = "A2A_TCK_STORE_BACKEND";
constexpr char kPostgresDsnEnv[] = "A2A_TCK_POSTGRES_DSN";
constexpr char kPostgresSchemaEnv[] = "A2A_TCK_POSTGRES_SCHEMA";
constexpr char kExtendedCardModeEnv[] = "A2A_TCK_EXTENDED_AGENT_CARD_MODE";

struct SutConfig final {
  std::string host = std::string(kDefaultHost);
  int port = kDefaultPort;
  int grpc_port = kDefaultPort + kGrpcPortOffset;
};

struct SutEndpoints final {
  std::string rest_url;
  std::string json_rpc_url;
  std::string grpc_url;
};

}  // namespace a2a::tests::sut
