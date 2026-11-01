// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/non_copyable.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/security_policy.h"
#include "a2a/server/server_interceptor.h"
#include "a2a/server/stores/store_factory.h"
#include "a2a/server/task_subscription_service.h"
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

struct SutAgentOptions final {
  bool asynchronous = false;
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

[[nodiscard]] core::Result<SutAgentOptions> ReadSutAgentOptions(bool asynchronous = false);

class SutAgent final : private core::NonCopyableOrMovable {
 public:
  SutAgent(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options = {});
  [[nodiscard]] std::shared_ptr<server::AgentExecutor> executor() const;
  [[nodiscard]] const lf::a2a::v1::AgentCard& agent_card() const noexcept;
  [[nodiscard]] std::shared_ptr<core::AgentCardProvider> card_provider() const;
  [[nodiscard]] std::shared_ptr<server::ServerInterceptor> interceptor() const;
  [[nodiscard]] std::shared_ptr<const server::SecurityPolicy> security_policy() const;
  [[nodiscard]] server::TaskStore* task_store() const;
  [[nodiscard]] server::Dispatcher& dispatcher() noexcept;
  core::Result<std::vector<lf::a2a::v1::StreamResponse>> BuildStreamingEvents(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext context);
  server::TaskSubscriptionService& TaskSubscriptions();
  void ShutdownSubscriptions();

 private:
  class Impl;
  std::shared_ptr<Impl> impl_;
};
}  // namespace a2a::tests::sut
