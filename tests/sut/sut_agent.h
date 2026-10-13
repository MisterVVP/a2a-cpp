// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <string>

#include "a2a/core/result.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/stores/store_factory.h"
#include "a2a/v1/a2a.pb.h"
#include "sut/tck_sut.h"

namespace a2a::tests::sut {

inline constexpr char kProfileEnv[] = "A2A_TCK_PROFILE";
inline constexpr char kAuthTokenEnv[] = "A2A_TCK_AUTH_TOKEN";
inline constexpr char kSigningKeyFileEnv[] = "A2A_TCK_SIGNING_KEY_FILE";
inline constexpr char kSigningKeyIdEnv[] = "A2A_TCK_SIGNING_KEY_ID";
inline constexpr char kTlsCertificateFileEnv[] = "A2A_TCK_TLS_CERT_FILE";
inline constexpr char kTlsKeyFileEnv[] = "A2A_TCK_TLS_KEY_FILE";
inline constexpr char kPublicHttpsBaseEnv[] = "A2A_TCK_PUBLIC_HTTPS_BASE";
inline constexpr char kAliceTokenEnv[] = "A2A_TCK_ALICE_TOKEN";
inline constexpr char kBobTokenEnv[] = "A2A_TCK_BOB_TOKEN";
inline constexpr std::string_view kSecurityProfile = "security";
inline constexpr std::string_view kAliceIdentity = "alice";
inline constexpr std::string_view kBobIdentity = "bob";
inline constexpr std::string_view kDefaultProfile = "default";
inline constexpr std::string_view kCapabilityNegativeProfile = "capability_negative";

struct SutAgentOptions final {
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

// All bindings share this executor, dispatcher, card provider and policy.
// Protocol validation and wire/error conversion remain in SDK adapters.
class SutAgent final {
 public:
  SutAgent(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options = {});
  ~SutAgent();
  SutAgent(const SutAgent&) = delete;
  SutAgent& operator=(const SutAgent&) = delete;
  SutAgent(SutAgent&&) = delete;
  SutAgent& operator=(SutAgent&&) = delete;

  [[nodiscard]] server::Dispatcher& dispatcher() noexcept;
  [[nodiscard]] const lf::a2a::v1::AgentCard& agent_card() const noexcept;
  void ShutdownSubscriptions();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace a2a::tests::sut
