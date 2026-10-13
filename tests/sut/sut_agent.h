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
inline constexpr std::string_view kDefaultProfile = "default";
inline constexpr std::string_view kCapabilityNegativeProfile = "capability_negative";

struct SutAgentOptions final {
  bool streaming = true;
  bool push_notifications = true;
  std::string auth_token;
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
