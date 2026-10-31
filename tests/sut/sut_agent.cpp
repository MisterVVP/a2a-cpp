// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/sut_agent.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include "a2a/core/agent_card/agent_card_builder.h"
#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/agent_card/agent_card_signing.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/server/security_policy.h"
#include "a2a/server/server_interceptor.h"
#include "a2a/server/tasks/task_authorization_service.h"
#include "example_support.h"

namespace a2a::tests::sut {
namespace {

constexpr std::string_view kUnreadableSigningKey = "cannot read configured card signing key";
constexpr std::string_view kInvalidSigningConfiguration = "card signing requires a key and key ID";
constexpr std::string_view kInvalidCallerTokens = "security profile requires two distinct caller tokens";
constexpr std::string_view kAgentName = "TCK SUT";
constexpr std::string_view kAgentRelease = "0.1.0";
constexpr std::string_view kAgentDescription = "Conformance-focused local SUT for A2A";
constexpr std::string_view kExtendedDescription = "Extended conformance-focused local SUT card for A2A";
constexpr std::string_view kBearerTokenKey = "bearer_token";
constexpr std::string_view kSecuritySchemeName = "sut_bearer";
constexpr std::string_view kBearerScheme = "bearer";
constexpr std::string_view kInvalidProfileMessage = "Unsupported A2A_TCK_PROFILE";
constexpr std::string_view kInvalidCardModeMessage = "Unsupported A2A_TCK_EXTENDED_AGENT_CARD_MODE";

bool AsyncTransportsSupported() {
  const auto* configured = std::getenv(kSutTransportsEnv);
  std::string_view names = configured == nullptr ? kGrpcTransport : configured;
  while (!names.empty()) {
    const auto comma = names.find(',');
    if (names.substr(0, comma) != kGrpcTransport) {
      return false;
    }
    if (comma == std::string_view::npos) {
      return true;
    }
    names.remove_prefix(comma + 1);
  }
  return false;
}

std::string_view EnvironmentValue(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string_view{} : std::string_view(value);
}

constexpr std::string_view kParentDirectorySegment = "..";
constexpr auto kFixturePollInterval = std::chrono::milliseconds(20);
constexpr std::size_t kMaxCredentialSize = 4096;

bool RequestsAuthorization(const lf::a2a::v1::SendMessageRequest& request) {
  return std::ranges::any_of(request.message().parts(),
                             [](const auto& part) { return part.text() == kAuthorizationTrigger; });
}

lf::a2a::v1::Message AuthorizationExplanation() {
  lf::a2a::v1::Message explanation;
  explanation.set_role(lf::a2a::v1::ROLE_AGENT);
  explanation.set_message_id(std::string(kAuthorizationStatusId));
  explanation.add_parts()->set_text(std::string(kAuthorizationExplanation));
  return explanation;
}

class SecurityExecutor final : public examples::ExampleExecutor {
 public:
  SecurityExecutor(server::TaskStore* store, server::PushNotificationStore* push_store,
                   std::filesystem::path credential_directory, std::string credential)
      : examples::ExampleExecutor(
            {.task_store = store, .push_store = push_store, .push_delivery = nullptr, .task_id_generator = {}}),
        store_(store),
        authorization_(store,
                       [credential = std::move(credential)](const auto&, std::string_view supplied) {
                         return !credential.empty() && supplied == credential;
                       }),
        credential_directory_(std::move(credential_directory)) {
    if (!credential_directory_.empty()) {
      worker_ = std::thread([this] { PollCredentials(); });
    }
  }

  ~SecurityExecutor() override { ShutdownSubscriptions(); }

  void ShutdownSubscriptions() {
    stopped_.store(true);
    if (worker_.joinable()) {
      worker_.join();
    }
    authorization_.Shutdown();
    examples::ExampleExecutor::ShutdownSubscriptions();
  }

  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                             server::RequestContext& context) override {
    if (!RequestsAuthorization(request) && !IsAuthorizationTask(request.message().task_id())) {
      return examples::ExampleExecutor::SendMessage(request, context);
    }
    const auto explanation = AuthorizationExplanation();
    auto task = request.message().task_id().empty() ? authorization_.RequestAuthorization(request, context, explanation)
                                                    : authorization_.ReceiveMessage(request);
    if (!task.ok()) {
      return task.error();
    }
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = std::move(task.value());
    return response;
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, server::RequestContext& context) override {
    if (!RequestsAuthorization(request) && !IsAuthorizationTask(request.message().task_id())) {
      return examples::ExampleExecutor::SendStreamingMessage(request, context);
    }
    const auto explanation = AuthorizationExplanation();
    return authorization_.SendStreamingMessage(request, context, explanation);
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                           server::RequestContext& context) override {
    return IsAuthorizationTask(request.id()) ? authorization_.Subscribe(request.id())
                                             : examples::ExampleExecutor::SubscribeTask(request, context);
  }

  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             server::RequestContext& context) override {
    return IsAuthorizationTask(request.id()) ? authorization_.Cancel(request.id())
                                             : examples::ExampleExecutor::CancelTask(request, context);
  }

 private:
  [[nodiscard]] bool IsAuthorizationTask(std::string_view id) const {
    if (id.empty()) {
      return false;
    }
    auto task = store_->Get(id);
    return task.ok() && server::TaskAuthorizationService::IsAuthorizationTask(task.value());
  }

  void PollCredentials() {
    while (!stopped_.load()) {
      server::ListTasksRequest query;
      query.status_filter = lf::a2a::v1::TASK_STATE_AUTH_REQUIRED;
      auto tasks = store_->List(query);
      if (tasks.ok()) {
        for (const auto& task : tasks.value().tasks) {
          const std::filesystem::path name(task.id());
          if (!server::TaskAuthorizationService::IsAuthorizationTask(task) || name != name.filename() ||
              name == std::filesystem::path(kParentDirectorySegment)) {
            continue;
          }
          std::ifstream fixture(credential_directory_ / name);
          std::string credential;
          for (char byte = 0; credential.size() <= kMaxCredentialSize && fixture.get(byte);) {
            credential.push_back(byte);
          }
          if (credential.size() > kMaxCredentialSize) {
            continue;
          }
          auto authorized = authorization_.Authorize(task.id(), credential);
          if (authorized.ok()) {
            (void)authorization_.Complete(task.id());
          }
        }
      }
      std::this_thread::sleep_for(kFixturePollInterval);
    }
  }

  server::TaskStore* store_;
  server::TaskAuthorizationService authorization_;
  std::filesystem::path credential_directory_;
  std::atomic_bool stopped_ = false;
  std::thread worker_;
};

class SutSecurityPolicy final : public server::SecurityPolicy {
 public:
  SutSecurityPolicy(std::string alice_token, std::string bob_token)
      : alice_token_(std::move(alice_token)), bob_token_(std::move(bob_token)) {}
  [[nodiscard]] core::Result<std::string> Authenticate(const server::RequestContext& context) const override {
    const auto token = context.auth_metadata.find(std::string(kBearerTokenKey));
    if (token == context.auth_metadata.end()) {
      return core::protocol_errors::Unauthenticated();
    }
    if (token->second == alice_token_) {
      return std::string(kAliceIdentity);
    }
    if (token->second == bob_token_) {
      return std::string(kBobIdentity);
    }
    return core::protocol_errors::Unauthenticated();
  }
  [[nodiscard]] bool AuthorizeOperation(const server::DispatchRequest& request,
                                        const server::RequestContext& context) const override {
    const auto* send = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
    return send == nullptr || !send->message().task_id().empty() || send->message().context_id() == context.principal;
  }
  [[nodiscard]] bool CanAccessTask(const lf::a2a::v1::Task& task,
                                   const server::RequestContext& context) const override {
    return task.context_id() == context.principal;
  }

  [[nodiscard]] std::optional<server::ListTasksRequest> ScopeTaskList(
      const server::ListTasksRequest& query, const server::RequestContext& context) const override {
    if (!query.context_id.empty() && query.context_id != context.principal) {
      return std::nullopt;
    }
    auto scoped = query;
    scoped.context_id = context.principal;
    scoped.history_length = 0;
    scoped.include_artifacts = false;
    return scoped;
  }

 private:
  std::string alice_token_;
  std::string bob_token_;
};

class SutPolicy final : public server::ServerInterceptor {
 public:
  explicit SutPolicy(const SutAgentOptions& options)
      : streaming_(options.streaming),
        push_notifications_(options.push_notifications),
        auth_token_(options.auth_token) {}

  core::Result<void> BeforeDispatch(const server::DispatchRequest& request, server::RequestContext& context) override {
    if (!auth_token_.empty()) {
      const auto token = context.auth_metadata.find(std::string(kBearerTokenKey));
      if (token == context.auth_metadata.end()) {
        return core::protocol_errors::Unauthenticated();
      }
      if (token->second != auth_token_) {
        return core::protocol_errors::PermissionDenied();
      }
    }
    const auto* send = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
    if (!push_notifications_ && send != nullptr && send->has_configuration() &&
        send->configuration().has_task_push_notification_config()) {
      return core::protocol_errors::PushNotificationNotSupported();
    }
    using Operation = server::DispatcherOperation;
    switch (request.operation) {
      case Operation::kSendStreamingMessage:
      case Operation::kSubscribeTask:
        if (!streaming_) {
          return core::protocol_errors::UnsupportedOperation();
        }
        break;
      case Operation::kCreateTaskPushNotificationConfig:
      case Operation::kGetTaskPushNotificationConfig:
      case Operation::kListTaskPushNotificationConfigs:
      case Operation::kDeleteTaskPushNotificationConfig:
        if (!push_notifications_) {
          return core::protocol_errors::PushNotificationNotSupported();
        }
        break;
      default:
        break;
    }
    return {};
  }

 private:
  bool streaming_;
  bool push_notifications_;
  std::string auth_token_;
};

lf::a2a::v1::AgentCard MaybeSignCard(lf::a2a::v1::AgentCard card, const SutAgentOptions& options) {
  if (options.signing_private_key.empty()) {
    return card;
  }
#ifdef A2A_HAS_AGENT_CARD_SIGNING
  card.clear_signatures();
  auto signed_card = core::SignAgentCard(card, options.signing_key_id, options.signing_private_key);
  if (!signed_card.ok()) {
    throw std::invalid_argument(std::string(signed_card.error().message()));
  }
  return std::move(signed_card.value());
#else
  constexpr std::string_view kSigningUnavailable = "Agent Card signing requires A2A_ENABLE_SIGNING=ON";
  throw std::invalid_argument(std::string(kSigningUnavailable));
#endif
}

lf::a2a::v1::AgentCard BuildCard(const SutEndpoints& endpoints, const SutAgentOptions& options) {
  auto card =
      core::AgentCardBuilder::ConformancePreset(
          {.rest_url = endpoints.rest_url, .json_rpc_url = endpoints.json_rpc_url, .grpc_url = endpoints.grpc_url},
          kAgentName, kAgentRelease, kAgentDescription)
          .WithPushNotifications(options.push_notifications)
          .WithExtendedAgentCard(options.extended_card_mode != kExtendedCardModeDisabled)
          .Build();
  if (options.asynchronous) {
    auto* interfaces = card.mutable_supported_interfaces();
    const auto removed = std::ranges::remove_if(*interfaces, [](const auto& interface) {
      return interface.protocol_binding() != core::protocol_bindings::kGrpc;
    });
    interfaces->erase(removed.begin(), removed.end());
  }
  card.mutable_capabilities()->set_streaming(options.streaming);
  if (!options.auth_token.empty() || options.security) {
    auto& scheme = (*card.mutable_security_schemes())[std::string(kSecuritySchemeName)];
    scheme.mutable_http_auth_security_scheme()->set_scheme(std::string(kBearerScheme));
    (*card.add_security_requirements()->mutable_schemes())[std::string(kSecuritySchemeName)];
  }
  return MaybeSignCard(std::move(card), options);
}

std::optional<lf::a2a::v1::AgentCard> ExtendedCard(const lf::a2a::v1::AgentCard& card, const SutAgentOptions& options) {
  if (options.extended_card_mode != kExtendedCardModeConfigured) {
    return std::nullopt;
  }
  auto extended = card;
  extended.set_description(std::string(kExtendedDescription));
  return MaybeSignCard(std::move(extended), options);
}

}  // namespace

core::Result<SutAgentOptions> ReadSutAgentOptions(bool asynchronous) {
  constexpr auto kUnsupportedTransports =
      "Only grpc is implemented in the async SUT; jsonrpc and http_json are not enabled yet";
  if (asynchronous && !AsyncTransportsSupported()) {
    return core::Error::Validation(kUnsupportedTransports);
  }
  SutAgentOptions options;
  const auto profile = EnvironmentValue(kProfileEnv);
  if (!profile.empty() && profile != kDefaultProfile && profile != kCapabilityNegativeProfile &&
      profile != kSecurityProfile) {
    return core::Error::Validation(std::string(kInvalidProfileMessage));
  }
  options.asynchronous = asynchronous;
  options.push_notifications = profile != kCapabilityNegativeProfile;
  options.streaming = options.push_notifications && !asynchronous;
  options.auth_token = EnvironmentValue(kAuthTokenEnv);
  options.security = profile == kSecurityProfile;
  options.alice_token = EnvironmentValue(kAliceTokenEnv);
  options.bob_token = EnvironmentValue(kBobTokenEnv);
  options.credential_directory = EnvironmentValue(kCredentialDirectoryEnv);
  options.task_credential = EnvironmentValue(kTaskCredentialEnv);
  const auto signing_key_path = EnvironmentValue(kSigningKeyFileEnv);
  options.signing_key_id = EnvironmentValue(kSigningKeyIdEnv);
  if (signing_key_path.empty() && !options.signing_key_id.empty()) {
    return core::Error::Validation(std::string(kInvalidSigningConfiguration));
  }
  if (!signing_key_path.empty()) {
    std::ifstream key_file{std::string(signing_key_path)};
    if (!key_file) {
      return core::Error::Validation(std::string(kUnreadableSigningKey));
    }
    options.signing_private_key.assign(std::istreambuf_iterator<char>(key_file), {});
    if (options.signing_private_key.empty() || options.signing_key_id.empty()) {
      return core::Error::Validation(std::string(kInvalidSigningConfiguration));
    }
  }
  if (options.security &&
      (options.alice_token.empty() || options.bob_token.empty() || options.alice_token == options.bob_token)) {
    return core::Error::Validation(std::string(kInvalidCallerTokens));
  }
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
        executor_(stores_.task_store.get(), stores_.push_store.get(), options.credential_directory,
                  options.task_credential),
        cards_(std::make_shared<core::StaticAgentCardProvider>(ExtendedCard(card_, options))),
        interceptor_(std::make_shared<SutPolicy>(options)),
        dispatcher_(&executor_, cards_) {
    dispatcher_.AddInterceptor(interceptor_);
    if (options.security) {
      security_ = std::make_shared<SutSecurityPolicy>(options.alice_token, options.bob_token);
      dispatcher_.SetSecurityPolicy(security_, stores_.task_store.get());
    }
  }

  server::stores::StoreBundle stores_;
  lf::a2a::v1::AgentCard card_;
  SecurityExecutor executor_;
  std::shared_ptr<core::AgentCardProvider> cards_;
  std::shared_ptr<server::ServerInterceptor> interceptor_;
  std::shared_ptr<const server::SecurityPolicy> security_;
  server::Dispatcher dispatcher_;
};

SutAgent::SutAgent(server::stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options)
    : impl_(std::make_shared<Impl>(std::move(stores), endpoints, options)) {}
std::shared_ptr<server::AgentExecutor> SutAgent::executor() const { return {impl_, &impl_->executor_}; }
server::Dispatcher& SutAgent::dispatcher() noexcept { return impl_->dispatcher_; }
const lf::a2a::v1::AgentCard& SutAgent::agent_card() const noexcept { return impl_->card_; }
std::shared_ptr<core::AgentCardProvider> SutAgent::card_provider() const { return impl_->cards_; }
std::shared_ptr<server::ServerInterceptor> SutAgent::interceptor() const { return impl_->interceptor_; }
std::shared_ptr<const server::SecurityPolicy> SutAgent::security_policy() const { return impl_->security_; }
server::TaskStore* SutAgent::task_store() const { return impl_->stores_.task_store.get(); }
void SutAgent::ShutdownSubscriptions() { impl_->executor_.ShutdownSubscriptions(); }

}  // namespace a2a::tests::sut
