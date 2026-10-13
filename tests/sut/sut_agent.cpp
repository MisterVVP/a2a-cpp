// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/sut_agent.h"

#include <cstdlib>
#include <optional>
#include <string_view>
#include <utility>

#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/server/server_interceptor.h"
#include "example_support.h"

namespace a2a::tests::sut {
namespace {

constexpr std::string_view kAgentName = "TCK SUT";
constexpr std::string_view kAgentRelease = "0.1.0";
constexpr std::string_view kAgentDescription = "Conformance-focused local SUT for A2A";
constexpr std::string_view kExtendedDescription = "Extended conformance-focused local SUT card for A2A";
constexpr std::string_view kBearerTokenKey = "bearer_token";
constexpr std::string_view kSecuritySchemeName = "sut_bearer";
constexpr std::string_view kBearerScheme = "bearer";
constexpr std::string_view kInvalidProfileMessage = "Unsupported A2A_TCK_PROFILE";
constexpr std::string_view kInvalidCardModeMessage = "Unsupported A2A_TCK_EXTENDED_AGENT_CARD_MODE";

std::string_view EnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string_view{} : std::string_view(value);
}

class SutPolicy final : public server::ServerInterceptor {
 public:
  explicit SutPolicy(SutAgentOptions options) : options_(std::move(options)) {}

  core::Result<void> BeforeDispatch(const server::DispatchRequest& request, server::RequestContext& context) override {
    if (!options_.auth_token.empty()) {
      const auto token = context.auth_metadata.find(std::string(kBearerTokenKey));
      if (token == context.auth_metadata.end()) {
        return core::protocol_errors::Unauthenticated();
      }
      if (token->second != options_.auth_token) {
        return core::protocol_errors::PermissionDenied();
      }
    }
    const auto* send = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
    if (!options_.push_notifications && send != nullptr && send->has_configuration() &&
        send->configuration().has_task_push_notification_config()) {
      return core::protocol_errors::PushNotificationNotSupported();
    }
    using Operation = server::DispatcherOperation;
    switch (request.operation) {
      case Operation::kSendStreamingMessage:
      case Operation::kSubscribeTask:
        if (!options_.streaming) {
          return core::protocol_errors::UnsupportedOperation();
        }
        break;
      case Operation::kCreateTaskPushNotificationConfig:
      case Operation::kGetTaskPushNotificationConfig:
      case Operation::kListTaskPushNotificationConfigs:
      case Operation::kDeleteTaskPushNotificationConfig:
        if (!options_.push_notifications) {
          return core::protocol_errors::PushNotificationNotSupported();
        }
        break;
      default:
        break;
    }
    return {};
  }

 private:
  SutAgentOptions options_;
};

lf::a2a::v1::AgentCard BuildCard(const SutEndpoints& endpoints, const SutAgentOptions& options) {
  auto card =
      core::AgentCardBuilder::ConformancePreset(
          {.rest_url = endpoints.rest_url, .json_rpc_url = endpoints.json_rpc_url, .grpc_url = endpoints.grpc_url},
          kAgentName, kAgentRelease, kAgentDescription)
          .WithPushNotifications(options.push_notifications)
          .WithExtendedAgentCard(options.extended_card_mode != kExtendedCardModeDisabled)
          .Build();
  card.mutable_capabilities()->set_streaming(options.streaming);
  if (!options.auth_token.empty()) {
    auto& scheme = (*card.mutable_security_schemes())[std::string(kSecuritySchemeName)];
    scheme.mutable_http_auth_security_scheme()->set_scheme(std::string(kBearerScheme));
    (*card.add_security_requirements()->mutable_schemes())[std::string(kSecuritySchemeName)];
  }
  return card;
}

std::optional<lf::a2a::v1::AgentCard> ExtendedCard(const lf::a2a::v1::AgentCard& card, const SutAgentOptions& options) {
  if (options.extended_card_mode != kExtendedCardModeConfigured) {
    return std::nullopt;
  }
  auto extended = card;
  extended.set_description(std::string(kExtendedDescription));
  return extended;
}

}  // namespace

core::Result<SutAgentOptions> ReadSutAgentOptions() {
  SutAgentOptions options;
  const auto profile = EnvironmentValue(kProfileEnv);
  if (!profile.empty() && profile != kDefaultProfile && profile != kCapabilityNegativeProfile) {
    return core::Error::Validation(std::string(kInvalidProfileMessage));
  }
  options.streaming = profile != kCapabilityNegativeProfile;
  options.push_notifications = options.streaming;
  options.auth_token = EnvironmentValue(kAuthTokenEnv);
  const auto card_mode = EnvironmentValue(kExtendedCardModeEnv);
  if (!card_mode.empty()) {
    options.extended_card_mode = card_mode;
  }
  if (options.extended_card_mode != kExtendedCardModeConfigured &&
      options.extended_card_mode != kExtendedCardModeDeclaredOnly &&
      options.extended_card_mode != kExtendedCardModeDisabled) {
    return core::Error::Validation(std::string(kInvalidCardModeMessage));
  }
  return options;
}

class SutAgent::Impl final {
 public:
  Impl(server::stores::StoreBundle stores, const SutEndpoints& endpoints, const SutAgentOptions& options)
      : stores_(std::move(stores)),
        card_(BuildCard(endpoints, options)),
        executor_({.task_store = stores_.task_store.get(),
                   .push_store = stores_.push_store.get(),
                   .push_delivery = nullptr,
                   .task_id_generator = {}}),
        dispatcher_(&executor_, std::make_shared<core::StaticAgentCardProvider>(ExtendedCard(card_, options))) {
    dispatcher_.AddInterceptor(std::make_shared<SutPolicy>(options));
  }

  server::stores::StoreBundle stores_;
  lf::a2a::v1::AgentCard card_;
  examples::ExampleExecutor executor_;
  server::Dispatcher dispatcher_;
};

SutAgent::SutAgent(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options)
    : impl_(std::make_unique<Impl>(std::move(stores), endpoints, options)) {}

SutAgent::~SutAgent() = default;
server::Dispatcher& SutAgent::dispatcher() noexcept { return impl_->dispatcher_; }
const lf::a2a::v1::AgentCard& SutAgent::agent_card() const noexcept { return impl_->card_; }
void SutAgent::ShutdownSubscriptions() { impl_->executor_.ShutdownSubscriptions(); }

}  // namespace a2a::tests::sut
