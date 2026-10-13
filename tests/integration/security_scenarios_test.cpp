// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "a2a/client/auth.h"
#include "a2a/client/discovery.h"
#include "a2a/client/grpc_transport.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/agent_card/agent_card_signing.h"
#include "support/security/fixtures.h"
#include "sut/security_executor.h"
#include "sut/sut_agent.h"

namespace {
using Binding = a2a::client::PreferredTransport;
using BindingPair = std::tuple<Binding, Binding>;
using namespace a2a::tests::security;
constexpr std::string_view kUnknownKeyMessage = "unknown fixture key";
constexpr char kCaFileEnv[] = "A2A_SECURITY_CA_FILE";
constexpr char kSutPortEnv[] = "A2A_SECURITY_SUT_PORT";
constexpr char kTlsHostEnv[] = "A2A_SECURITY_TLS_HOST";
constexpr char kHttpsPortEnv[] = "A2A_SECURITY_HTTPS_PORT";
constexpr char kCredentialDirectoryEnv[] = "A2A_TCK_CREDENTIAL_DIRECTORY";
constexpr char kSecurityModeEnv[] = "A2A_SECURITY_MODE";
constexpr char kPublicKeyFileEnv[] = "A2A_SECURITY_CARD_PUBLIC_KEY";
constexpr char kSigningKeyIdEnv[] = "A2A_TCK_SIGNING_KEY_ID";
constexpr std::string_view kRestBindingName = "HttpJson";
constexpr std::string_view kRpcBindingName = "JsonRpc";
constexpr std::string_view kGrpcBindingName = "Grpc";
constexpr std::string_view kBindingPairSeparator = "To";
constexpr std::array kBindings{Binding::kRest, Binding::kJsonRpc, Binding::kGrpc};
constexpr std::string_view kPlaintextBaseUrl = "http://127.0.0.1:";
constexpr std::string_view kHttpsScheme = "https://";
constexpr std::string_view kLoopbackHost = "127.0.0.1";
constexpr std::string_view kEndpointSeparator = ":";
constexpr std::string_view kAlice = "alice";
constexpr std::string_view kBob = "bob";
constexpr std::string_view kMessageId = "sdk-security-message";
constexpr std::string_view kFollowupId = "sdk-security-followup";
constexpr std::string_view kText = "SDK security scenario";
constexpr std::string_view kMissingTaskId = "sdk-security-missing-task";
constexpr std::string_view kPushId = "sdk-security-push";
constexpr std::string_view kPushUrl = "https://example.invalid/sdk-security";
constexpr auto kStreamTimeout = std::chrono::seconds(5);
constexpr auto kCallTimeout = std::chrono::seconds(2);
constexpr auto kKeyLifetime = std::chrono::hours(1);
constexpr std::size_t kHistoryCount = 2;
constexpr std::string_view kPlaintextMode = "plaintext";
constexpr std::string_view kTrustedMode = "trusted";

std::string BindingName(Binding binding) {
  switch (binding) {
    case Binding::kRest:
      return std::string(kRestBindingName);
    case Binding::kJsonRpc:
      return std::string(kRpcBindingName);
    case Binding::kGrpc:
      return std::string(kGrpcBindingName);
  }
  return {};
}

std::string Environment(const char* key) {
  const char* value = std::getenv(key);
  return value == nullptr ? std::string{} : std::string(value);
}
std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), {}};
}
lf::a2a::v1::SendMessageRequest Message(std::string_view owner, std::string_view text = kText) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_message_id(std::string(kMessageId));
  request.mutable_message()->set_context_id(std::string(owner));
  request.mutable_message()->add_parts()->set_text(std::string(text));
  return request;
}
a2a::client::CallOptions Options(std::string_view token) {
  a2a::client::CallOptions options;
  options.timeout = kCallTimeout;
  options.extensions = {std::string(a2a::tests::sut::kRequiredExtensionUri)};
  options.credential_provider = std::make_shared<a2a::client::BearerTokenCredentialProvider>(std::string(token));
  const auto ca = Environment(kCaFileEnv);
  if (!ca.empty()) {
    options.tls = a2a::http::TlsOptions{.ca_bundle_path = ca, .require_tls13 = true};
  }
  return options;
}

class Recorder final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& event) override {
    std::lock_guard lock(mutex_);
    events_.push_back(event);
    ready_.notify_all();
  }
  void OnError(const a2a::core::Error& error) override {
    std::lock_guard lock(mutex_);
    error_ = error;
    finished_ = true;
    ready_.notify_all();
  }
  void OnCompleted() override {
    std::lock_guard lock(mutex_);
    finished_ = true;
    ready_.notify_all();
  }
  bool Wait(bool completion) {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, kStreamTimeout,
                           [&] { return completion ? finished_ : !events_.empty() || finished_; });
  }
  std::vector<lf::a2a::v1::StreamResponse> Events() const {
    std::lock_guard lock(mutex_);
    return events_;
  }
  std::optional<a2a::core::Error> Error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }
  bool Finished() const {
    std::lock_guard lock(mutex_);
    return finished_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::optional<a2a::core::Error> error_;
  bool finished_ = false;
};

class SecurityScenariosTest : public testing::TestWithParam<BindingPair> {
 protected:
  void SetUp() override {
    const auto port = Environment(kSutPortEnv);
    ASSERT_FALSE(port.empty());
    const auto host = Environment(kTlsHostEnv);
    base_.append(host.empty() ? kPlaintextBaseUrl : kHttpsScheme);
    if (!host.empty()) {
      base_.append(host).append(kEndpointSeparator).append(Environment(kHttpsPortEnv));
    } else {
      base_.append(port);
    }
    grpc_url_.append(host.empty() ? kLoopbackHost : host)
        .append(kEndpointSeparator)
        .append(std::to_string(std::stoi(port) + 1));
    source_ = MakeClient(std::get<0>(GetParam()));
    target_ = MakeClient(std::get<1>(GetParam()));
  }
  [[nodiscard]] std::unique_ptr<a2a::client::A2AClient> MakeClient(Binding binding) const {
    a2a::client::ResolvedInterface iface{
        .transport = binding, .url = base_, .security_requirements = {}, .security_schemes = {}};
    std::unique_ptr<a2a::client::ClientTransport> transport;
    if (binding == Binding::kRest) {
      iface.url.append(a2a::tests::sut::kRestApiBasePath);
      transport = a2a::client::HttpJsonTransport::CreateDefault(iface);
    } else if (binding == Binding::kJsonRpc) {
      iface.url.append(a2a::tests::sut::kJsonRpcPath);
      transport = a2a::client::JsonRpcTransport::CreateDefault(iface);
    } else {
      iface.url = grpc_url_;
      auto credentials = grpc::InsecureChannelCredentials();
      const auto ca = Environment(kCaFileEnv);
      if (!ca.empty()) {
        grpc::SslCredentialsOptions ssl;
        ssl.pem_root_certs = ReadFile(ca);
        credentials = grpc::SslCredentials(ssl);
      }
      transport = std::make_unique<a2a::client::GrpcTransport>(iface, grpc::CreateChannel(grpc_url_, credentials));
    }
    return std::make_unique<a2a::client::A2AClient>(std::move(transport));
  }
  void CheckHiddenRead(std::string_view id, const a2a::client::CallOptions& bob) {
    lf::a2a::v1::GetTaskRequest get;
    get.set_id(std::string(id));
    auto hidden = target_->GetTask(get, bob);
    ASSERT_FALSE(hidden.ok());
    get.set_id(std::string(kMissingTaskId));
    auto missing = target_->GetTask(get, bob);
    ASSERT_FALSE(missing.ok());
    EXPECT_EQ(hidden.error().http_status(), missing.error().http_status());
    EXPECT_EQ(hidden.error().protocol_code(), missing.error().protocol_code());
  }
  static void CheckStreamFailure(Recorder& recorder) {
    ASSERT_TRUE(recorder.Wait(true));
    EXPECT_TRUE(recorder.Error().has_value());
  }
  void CheckHiddenStreams(std::string_view id, const a2a::client::CallOptions& bob) {
    lf::a2a::v1::GetTaskRequest get;
    get.set_id(std::string(id));
    auto followup = Message(kAlice);
    followup.mutable_message()->set_task_id(std::string(id));
    Recorder stream;
    auto subscribed = target_->SubscribeTask(get, stream, bob);
    if (subscribed.ok()) {
      CheckStreamFailure(stream);
    }
    Recorder send_stream;
    auto sent = target_->SendStreamingMessage(followup, send_stream, bob);
    if (sent.ok()) {
      CheckStreamFailure(send_stream);
    }
  }
  void CheckHiddenOperations(std::string_view id) {
    auto bob = Options(kBobToken);
    CheckHiddenRead(id, bob);
    lf::a2a::v1::CancelTaskRequest cancel;
    cancel.set_id(std::string(id));
    EXPECT_FALSE(target_->CancelTask(cancel, bob).ok());
    auto followup = Message(kAlice);
    followup.mutable_message()->set_task_id(std::string(id));
    EXPECT_FALSE(target_->SendMessage(followup, bob).ok());
    CheckHiddenStreams(id, bob);
    CheckHiddenPush(id, bob);
  }
  std::string CreateTask(std::string_view owner, const a2a::client::CallOptions& options) {
    auto created = source_->SendMessage(Message(owner), options);
    EXPECT_TRUE(created.ok());
    if (!created.ok()) {
      return {};
    }
    EXPECT_TRUE(created.value().has_task());
    return created.value().task().id();
  }
  void CheckOwnedList(const a2a::client::CallOptions& options) {
    a2a::client::ListTasksRequest list;
    list.page_size = a2a::server::kMaxListTasksPageSize;
    auto listed = target_->ListTasks(list, options);
    ASSERT_TRUE(listed.ok());
    for (const auto& task : listed.value().tasks) {
      EXPECT_EQ(task.context_id(), kBob);
    }
    EXPECT_TRUE(listed.value().next_page_token.empty());
  }
  void CheckHiddenPush(std::string_view id, const a2a::client::CallOptions& options) {
    lf::a2a::v1::TaskPushNotificationConfig create;
    create.set_task_id(std::string(id));
    create.set_id(std::string(kPushId));
    create.set_url(std::string(kPushUrl));
    EXPECT_FALSE(target_->CreateTaskPushNotificationConfig(create, options).ok());
    lf::a2a::v1::GetTaskPushNotificationConfigRequest get;
    get.set_task_id(std::string(id));
    get.set_id(std::string(kPushId));
    EXPECT_FALSE(target_->GetTaskPushNotificationConfig(get, options).ok());
    lf::a2a::v1::ListTaskPushNotificationConfigsRequest list;
    list.set_task_id(std::string(id));
    EXPECT_FALSE(target_->ListTaskPushNotificationConfigs(list, options).ok());
    lf::a2a::v1::DeleteTaskPushNotificationConfigRequest remove;
    remove.set_task_id(std::string(id));
    remove.set_id(std::string(kPushId));
    EXPECT_FALSE(target_->DeleteTaskPushNotificationConfig(remove, options).ok());
  }
  static std::string PendingTaskId(const Recorder& stream) {
    const auto events = stream.Events();
    EXPECT_FALSE(events.empty());
    if (events.empty()) {
      return {};
    }
    EXPECT_TRUE(events.front().has_task());
    EXPECT_EQ(events.front().task().status().state(), lf::a2a::v1::TASK_STATE_AUTH_REQUIRED);
    EXPECT_FALSE(events.front().task().status().message().parts().empty());
    EXPECT_FALSE(stream.Finished());
    return events.front().task().id();
  }
  void ReceivePendingMessage(std::string_view id, const a2a::client::CallOptions& options) {
    auto followup = Message(kAlice);
    followup.mutable_message()->set_task_id(std::string(id));
    followup.mutable_message()->set_message_id(std::string(kFollowupId));
    auto received = target_->SendMessage(followup, options);
    ASSERT_TRUE(received.ok());
    EXPECT_EQ(received.value().task().status().state(), lf::a2a::v1::TASK_STATE_AUTH_REQUIRED);
    EXPECT_EQ(received.value().task().history_size(), kHistoryCount);
  }
  static void CheckStreamCompletion(Recorder& recorder) {
    ASSERT_TRUE(recorder.Wait(true));
    EXPECT_FALSE(recorder.Error().has_value());
  }
  void CheckCompletedStreams(std::string_view id, const a2a::client::CallOptions& options, Recorder& active,
                             Recorder& subscribed) {
    CheckStreamCompletion(active);
    CheckStreamCompletion(subscribed);
    lf::a2a::v1::GetTaskRequest get;
    get.set_id(std::string(id));
    auto task = target_->GetTask(get, options);
    ASSERT_TRUE(task.ok());
    EXPECT_EQ(task.value().id(), id);
    EXPECT_EQ(task.value().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  }
  static std::string CreateStreamingTask(a2a::client::A2AClient& client, std::string_view owner,
                                         const a2a::client::CallOptions& options) {
    Recorder recorder;
    auto sent = client.SendStreamingMessage(Message(owner), recorder, options);
    EXPECT_TRUE(sent.ok());
    if (!sent.ok()) {
      return {};
    }
    const bool completed = recorder.Wait(true);
    EXPECT_TRUE(completed);
    if (!completed) {
      return {};
    }
    EXPECT_FALSE(recorder.Error().has_value());
    const auto events = recorder.Events();
    EXPECT_FALSE(events.empty());
    if (events.empty()) {
      return {};
    }
    EXPECT_EQ(events.front().status_update().context_id(), owner);
    return events.front().status_update().task_id();
  }
  std::string base_;
  std::string grpc_url_;
  std::unique_ptr<a2a::client::A2AClient> source_, target_;
};

TEST_P(SecurityScenariosTest, CallerPermissionsHideTasksAcrossBindings) {
  auto alice = Options(kAliceToken);
  auto bob = Options(kBobToken);
  const auto id = CreateTask(kAlice, alice);
  ASSERT_FALSE(id.empty());
  ASSERT_FALSE(CreateTask(kBob, bob).empty());
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(id);
  EXPECT_TRUE(target_->GetTask(get, alice).ok());
  EXPECT_FALSE(target_->GetTask(get, Options(kUnknownToken)).ok());
  EXPECT_FALSE(source_->SendMessage(Message(kBob), alice).ok());
  CheckHiddenOperations(id);
  CheckOwnedList(bob);
}
TEST_P(SecurityScenariosTest, NewStreamsWithReusedMessageIdsStayInCallerScope) {
  const auto bob_id = CreateStreamingTask(*source_, kBob, Options(kBobToken));
  const auto alice_id = CreateStreamingTask(*target_, kAlice, Options(kAliceToken));
  ASSERT_FALSE(bob_id.empty());
  ASSERT_FALSE(alice_id.empty());
  EXPECT_NE(bob_id, alice_id);
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(bob_id);
  EXPECT_TRUE(target_->GetTask(get, Options(kBobToken)).ok());
  EXPECT_FALSE(target_->GetTask(get, Options(kAliceToken)).ok());
}

TEST_P(SecurityScenariosTest, AuthorizationKeepsStreamsOpenAndAcceptsPendingMessages) {
  auto alice = Options(kAliceToken);
  Recorder active;
  auto sent = source_->SendStreamingMessage(Message(kAlice, a2a::tests::sut::kAuthorizationTrigger), active, alice);
  ASSERT_TRUE(sent.ok());
  ASSERT_TRUE(active.Wait(false));
  const auto id = PendingTaskId(active);
  ASSERT_FALSE(id.empty());
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(id);
  Recorder subscribed;
  auto subscription = target_->SubscribeTask(get, subscribed, alice);
  ASSERT_TRUE(subscription.ok());
  ASSERT_TRUE(subscribed.Wait(false));
  ReceivePendingMessage(id, alice);
  EXPECT_FALSE(active.Finished());
  EXPECT_FALSE(subscribed.Finished());
  {
    std::ofstream fixture(std::filesystem::path(Environment(kCredentialDirectoryEnv)) / id);
    fixture << kTaskCredential;
  }
  CheckCompletedStreams(id, alice, active, subscribed);
}

TEST_P(SecurityScenariosTest, TlsTrustAndServerIdentity) {
  const auto mode = Environment(kSecurityModeEnv);
  auto response = target_->SendMessage(Message(kAlice), Options(kAliceToken));
  if (mode == kPlaintextMode || mode == kTrustedMode) {
    EXPECT_TRUE(response.ok());
  } else {
    EXPECT_FALSE(response.ok());
  }
}

TEST_P(SecurityScenariosTest, SignedDiscoveryChecksCurrentKeyPolicy) {
  const auto now = std::chrono::system_clock::now();
  auto options = Options(kAliceToken);
  a2a::client::DiscoveryClient discovery(a2a::client::MakeDefaultHttpFetcher(options.tls));
  const auto public_key = ReadFile(Environment(kPublicKeyFileEnv));
  bool revoked = false;
  a2a::core::AgentCardKeyResolver resolver =
      [&](std::string_view id) -> a2a::core::Result<a2a::core::AgentCardVerificationKey> {
    if (id != Environment(kSigningKeyIdEnv)) {
      return a2a::core::Error::Validation(std::string(kUnknownKeyMessage));
    }
    return a2a::core::AgentCardVerificationKey{.public_key_pem = public_key,
                                               .valid_from = now - kKeyLifetime,
                                               .valid_until = now + kKeyLifetime,
                                               .revoked = revoked};
  };
  discovery.SetAgentCardVerifier(
      [&](std::string_view json) { return a2a::core::VerifyAgentCardJson(json, resolver, now); });
  auto card = discovery.Fetch(base_);
  ASSERT_TRUE(card.ok());
  EXPECT_FALSE(card.value().signatures().empty());
  revoked = true;
  EXPECT_FALSE(discovery.Fetch(base_).ok());
}

INSTANTIATE_TEST_SUITE_P(AllBindings, SecurityScenariosTest,
                         testing::Combine(testing::ValuesIn(kBindings), testing::ValuesIn(kBindings)),
                         [](const testing::TestParamInfo<BindingPair>& info) {
                           return BindingName(std::get<0>(info.param))
                               .append(kBindingPairSeparator)
                               .append(BindingName(std::get<1>(info.param)));
                         });
}  // namespace
