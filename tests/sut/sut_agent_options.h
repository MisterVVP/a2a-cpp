// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

#include "a2a/core/result.h"
#include "sut/tck_sut.h"

namespace a2a::tests::sut {
inline constexpr std::string_view kAuthorizationTrigger = "request-authorization";
inline constexpr std::string_view kAuthorizationExplanation =
    "Supply the task credential using the out-of-band fixture directory.";
inline constexpr std::string_view kAuthorizationStatusId = "authorization-request";
inline constexpr char kCredentialDirectoryEnv[] = "A2A_TCK_CREDENTIAL_DIRECTORY";
inline constexpr char kTaskCredentialEnv[] = "A2A_TCK_TASK_CREDENTIAL";

inline constexpr char kProfileEnv[] = "A2A_TCK_PROFILE";
inline constexpr char kAuthTokenEnv[] = "A2A_TCK_AUTH_TOKEN";
inline constexpr char kSigningKeyFileEnv[] = "A2A_TCK_SIGNING_KEY_FILE";
inline constexpr char kSigningKeyIdEnv[] = "A2A_TCK_SIGNING_KEY_ID";
inline constexpr char kTlsCertificateFileEnv[] = "A2A_TCK_TLS_CERT_FILE";
inline constexpr char kTlsKeyFileEnv[] = "A2A_TCK_TLS_KEY_FILE";
inline constexpr char kPublicHttpsBaseEnv[] = "A2A_TCK_PUBLIC_HTTPS_BASE";
inline constexpr char kPublicGrpcTargetEnv[] = "A2A_TCK_PUBLIC_GRPC_TARGET";
inline constexpr char kAliceTokenEnv[] = "A2A_TCK_ALICE_TOKEN";
inline constexpr char kBobTokenEnv[] = "A2A_TCK_BOB_TOKEN";
inline constexpr std::string_view kSecurityProfile = "security";
inline constexpr std::string_view kAliceIdentity = "alice";
inline constexpr std::string_view kBobIdentity = "bob";
inline constexpr std::string_view kDefaultProfile = "default";
inline constexpr std::string_view kCapabilityNegativeProfile = "capability_negative";

inline constexpr char kSutTransportsEnv[] = "A2A_TCK_TRANSPORTS";
inline constexpr std::string_view kGrpcTransport = "grpc";

struct SutTransports final {
  bool grpc = true;
  bool jsonrpc = true;
  bool http_json = true;
};

[[nodiscard]] core::Result<SutTransports> ParseSutTransports(std::string_view names);

struct SutAgentOptions final {
  SutTransports transports = {};
  bool streaming = true;
  bool push_notifications = true;
  std::string auth_token;
  bool security = false;
  std::string alice_token = {};
  std::string bob_token = {};
  std::string credential_directory = {};
  std::string task_credential = {};
  std::string signing_key_id = {};
  std::string signing_private_key = {};
  std::string extended_card_mode = std::string(kExtendedCardModeConfigured);
};

[[nodiscard]] core::Result<SutAgentOptions> ReadSutAgentOptions();

}  // namespace a2a::tests::sut
