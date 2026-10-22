// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <google/protobuf/util/message_differencer.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/grpc_transport.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/version.h"
#include "sut/sut_agent.h"

namespace {

using Binding = a2a::client::PreferredTransport;
constexpr std::array kBindings{Binding::kRest, Binding::kJsonRpc, Binding::kGrpc};
constexpr char kHostEnv[] = "A2A_BINDING_SUT_HOST";
constexpr char kPortEnv[] = "A2A_BINDING_SUT_PORT";
constexpr char kMessageId[] = "binding-message";
constexpr char kMessageResponseId[] = "message-response-binding";
constexpr char kCompletedMessageId[] = "complete-task-binding";
constexpr char kStreamMessageId[] = "binding-stream-message";
constexpr char kContextId[] = "binding-context";
constexpr char kText[] = "binding equivalence";
constexpr char kPushId[] = "binding-push-config";
constexpr char kPushUrl[] = "https://example.invalid/binding-push";
constexpr char kMissingTaskId[] = "binding-nonexistent-task";
constexpr char kCursorFirstMessage[] = "cursor-first-message";
constexpr char kCursorSecondMessage[] = "cursor-second-message";
constexpr char kInvalidCursor[] = "a2a1_invalid";
constexpr char kLegacyOffset[] = "1";
constexpr char kWrongToken[] = "invalid-binding-token";
constexpr char kAuthorization[] = "Authorization";
constexpr char kBearerPrefix[] = "Bearer ";
constexpr auto kStreamTimeout = std::chrono::seconds(5);
constexpr int kGrpcNotFound = 5;
constexpr int kGrpcUnimplemented = 12;
constexpr int kGrpcUnauthenticated = 16;
constexpr int kGrpcPermissionDenied = 7;
constexpr std::size_t kArtifactCount = 4;

std::string EnvironmentValue(std::string_view name) {
  const char* value = std::getenv(std::string(name).c_str());
  return value == nullptr ? std::string{} : std::string(value);
}

std::string BindingName(Binding binding) {
  switch (binding) {
    case Binding::kRest:
      return "HttpJson";
    case Binding::kJsonRpc:
      return "JsonRpc";
    case Binding::kGrpc:
      return "Grpc";
  }
  return {};
}

lf::a2a::v1::SendMessageRequest Message(std::string_view message_id = kMessageId) {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
  request.mutable_message()->set_message_id(std::string(message_id));
  request.mutable_message()->set_context_id(kContextId);
  request.mutable_message()->add_parts()->set_text(kText);
  return request;
}

class EventRecorder final : public a2a::client::StreamObserver {
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
  bool WaitForEvent() {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, kStreamTimeout, [this] { return !events_.empty() || finished_; });
  }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, kStreamTimeout, [this] { return finished_; });
  }
  std::vector<lf::a2a::v1::StreamResponse> events() const {
    std::lock_guard lock(mutex_);
    return events_;
  }
  std::optional<a2a::core::Error> error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::vector<lf::a2a::v1::StreamResponse> events_;
  std::optional<a2a::core::Error> error_;
  bool finished_ = false;
};

using BindingPair = std::tuple<Binding, Binding>;
class SharedBindingTest : public testing::TestWithParam<BindingPair> {
 protected:
  void SetUp() override {
    const auto host = EnvironmentValue(kHostEnv);
    ASSERT_FALSE(host.empty()) << "Run through tests/scripts/run_binding_scenarios.py";
    const auto port = EnvironmentValue(kPortEnv);
    std::string base;
    base.append("http://").append(host).append(":").append(port);
    rest_url_ = base;
    rest_url_.append(a2a::tests::sut::kRestApiBasePath);
    rpc_url_ = base;
    rpc_url_.append(a2a::tests::sut::kJsonRpcPath);
    grpc_url_.append(host).append(":").append(std::to_string(std::stoi(port) + a2a::tests::sut::kGrpcPortOffset));
    options_.extensions = {std::string(a2a::tests::sut::kRequiredExtensionUri)};
    const auto token = EnvironmentValue(a2a::tests::sut::kAuthTokenEnv);
    if (!token.empty()) {
      options_.headers[std::string(kAuthorization)] = std::string(kBearerPrefix).append(token);
    }
    source_ = MakeClient(std::get<0>(GetParam()));
    target_ = MakeClient(std::get<1>(GetParam()));
  }

  std::unique_ptr<a2a::client::A2AClient> MakeClient(Binding binding) const {
    a2a::client::ResolvedInterface iface{
        .transport = binding, .url = rest_url_, .security_requirements = {}, .security_schemes = {}};
    std::unique_ptr<a2a::client::ClientTransport> transport;
    switch (binding) {
      case Binding::kRest:
        transport = a2a::client::HttpJsonTransport::CreateDefault(iface);
        break;
      case Binding::kJsonRpc:
        iface.url = rpc_url_;
        transport = a2a::client::JsonRpcTransport::CreateDefault(iface);
        break;
      case Binding::kGrpc:
        iface.url = grpc_url_;
        transport = std::make_unique<a2a::client::GrpcTransport>(
            iface, grpc::CreateChannel(grpc_url_, grpc::InsecureChannelCredentials()));
        break;
    }
    return std::make_unique<a2a::client::A2AClient>(std::move(transport));
  }

  static void ExpectTaskEqual(const lf::a2a::v1::Task& expected, const lf::a2a::v1::Task& actual) {
    EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected, actual)) << actual.DebugString();
  }

  static void ExpectProtocolFailure(const a2a::core::Error& error, Binding binding, std::string_view rpc_code,
                                    int grpc_code, int http_status) {
    switch (binding) {
      case Binding::kGrpc:
        EXPECT_EQ(error.protocol_code(), std::to_string(grpc_code));
        break;
      case Binding::kJsonRpc:
        EXPECT_EQ(error.protocol_code(), rpc_code);
        EXPECT_EQ(error.http_status(), a2a::core::http::kStatusOk);
        break;
      case Binding::kRest:
        EXPECT_EQ(error.http_status(), http_status);
        break;
    }
  }

  static void ExpectEventsEqual(const std::vector<lf::a2a::v1::StreamResponse>& expected,
                                const std::vector<lf::a2a::v1::StreamResponse>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    ASSERT_FALSE(expected.empty());
    for (std::size_t index = 0; index < expected.size(); ++index) {
      SCOPED_TRACE(index);
      EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected[index], actual[index]));
    }
  }

  static void ExpectAuthFailure(const a2a::core::Error& error, Binding binding, int grpc_status, int http_status) {
    if (binding == Binding::kGrpc) {
      EXPECT_EQ(error.protocol_code(), std::to_string(grpc_status));
    } else {
      EXPECT_EQ(error.http_status(), http_status);
    }
  }

  void CheckUnauthorizedTaskAccess(const lf::a2a::v1::GetTaskRequest& get) {
    auto unauthenticated = options_;
    unauthenticated.headers.clear();
    const auto missing = target_->GetTask(get, unauthenticated);
    ASSERT_FALSE(missing.ok());
    auto denied = unauthenticated;
    denied.headers[std::string(kAuthorization)] = std::string(kBearerPrefix).append(kWrongToken);
    const auto wrong = target_->GetTask(get, denied);
    ASSERT_FALSE(wrong.ok());
    const auto binding = std::get<1>(GetParam());
    ExpectAuthFailure(missing.error(), binding, kGrpcUnauthenticated, a2a::core::http::kStatusUnauthorized);
    ExpectAuthFailure(wrong.error(), binding, kGrpcPermissionDenied, a2a::core::http::kStatusForbidden);
  }

  void CheckRejectedVersions(const lf::a2a::v1::GetTaskRequest& get, std::string_view unsupported) {
    for (const auto version : {unsupported, std::string_view{}}) {
      SCOPED_TRACE(version);
      auto options = options_;
      options.protocol_version = version;
      const auto rejected = target_->GetTask(get, options);
      ASSERT_FALSE(rejected.ok());
      ExpectProtocolFailure(rejected.error(), std::get<1>(GetParam()), a2a::core::protocol_codes::kVersionNotSupported,
                            kGrpcUnimplemented, a2a::core::http::kStatusBadRequest);
    }
  }

  a2a::client::CallOptions options_;
  std::unique_ptr<a2a::client::A2AClient> source_;
  std::unique_ptr<a2a::client::A2AClient> target_;

 private:
  std::string rest_url_;
  std::string rpc_url_;
  std::string grpc_url_;
};

// BIND-EQUIV-001/002: compare the exact same stored identity, state, history,
// messages and artifacts. No wire normalization or field stripping is needed.
TEST_P(SharedBindingTest, CrossBindingTaskHistoryArtifactsAndCancellation) {
  const auto sent = source_->SendMessage(Message(), options_);
  ASSERT_TRUE(sent.ok()) << sent.error().message();
  ASSERT_TRUE(sent.value().has_task());
  const auto& task = sent.value().task();
  EXPECT_EQ(task.context_id(), kContextId);
  EXPECT_EQ(task.artifacts_size(), kArtifactCount);
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(task.id());
  const auto loaded = target_->GetTask(get, options_);
  ASSERT_TRUE(loaded.ok()) << loaded.error().message();
  ExpectTaskEqual(task, loaded.value());
  auto continuation = Message();
  continuation.mutable_message()->set_task_id(task.id());
  const auto updated = target_->SendMessage(continuation, options_);
  ASSERT_TRUE(updated.ok()) << updated.error().message();
  const auto reread = source_->GetTask(get, options_);
  ASSERT_TRUE(reread.ok()) << reread.error().message();
  ExpectTaskEqual(updated.value().task(), reread.value());
  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id(task.id());
  const auto canceled = target_->CancelTask(cancel, options_);
  ASSERT_TRUE(canceled.ok()) << canceled.error().message();
  EXPECT_EQ(canceled.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
  const auto final = source_->GetTask(get, options_);
  ASSERT_TRUE(final.ok()) << final.error().message();
  ExpectTaskEqual(canceled.value(), final.value());
}

TEST_P(SharedBindingTest, DirectMessagesHaveEquivalentContent) {
  const auto first = source_->SendMessage(Message(kMessageResponseId), options_);
  const auto second = target_->SendMessage(Message(kMessageResponseId), options_);
  ASSERT_TRUE(first.ok()) << first.error().message();
  ASSERT_TRUE(second.ok()) << second.error().message();
  ASSERT_TRUE(first.value().has_message());
  ASSERT_TRUE(second.value().has_message());
  // These separate sends generate different task/message IDs; compare all
  // other fields and explicitly verify the returned identity references.
  auto expected = first.value().message();
  auto actual = second.value().message();
  EXPECT_FALSE(expected.task_id().empty());
  EXPECT_FALSE(actual.task_id().empty());
  expected.clear_message_id();
  expected.clear_task_id();
  actual.clear_message_id();
  actual.clear_task_id();
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(expected, actual));
}

TEST_P(SharedBindingTest, PushConfigurationIsSharedAcrossBindings) {
  const auto sent = source_->SendMessage(Message(), options_);
  ASSERT_TRUE(sent.ok()) << sent.error().message();
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_task_id(sent.value().task().id());
  config.set_id(kPushId);
  config.set_url(kPushUrl);
  const auto created = source_->CreateTaskPushNotificationConfig(config, options_);
  ASSERT_TRUE(created.ok()) << created.error().message();
  lf::a2a::v1::GetTaskPushNotificationConfigRequest get;
  get.set_task_id(config.task_id());
  get.set_id(kPushId);
  const auto loaded = target_->GetTaskPushNotificationConfig(get, options_);
  ASSERT_TRUE(loaded.ok()) << loaded.error().message();
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(created.value(), loaded.value()));
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list;
  list.set_task_id(config.task_id());
  const auto listed = target_->ListTaskPushNotificationConfigs(list, options_);
  ASSERT_TRUE(listed.ok()) << listed.error().message();
  ASSERT_EQ(listed.value().configs_size(), 1);
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(created.value(), listed.value().configs(0)));
  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest remove;
  remove.set_task_id(config.task_id());
  remove.set_id(kPushId);
  ASSERT_TRUE(target_->DeleteTaskPushNotificationConfig(remove, options_).ok());
  const auto empty = source_->ListTaskPushNotificationConfigs(list, options_);
  ASSERT_TRUE(empty.ok()) << empty.error().message();
  EXPECT_EQ(empty.value().configs_size(), 0);
}

TEST_P(SharedBindingTest, LiveSubscriptionPreservesTaskAcrossBindings) {
  const auto sent = source_->SendMessage(Message(), options_);
  ASSERT_TRUE(sent.ok()) << sent.error().message();
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(sent.value().task().id());
  EventRecorder recorder;
  auto handle = target_->SubscribeTask(get, recorder, options_);
  ASSERT_TRUE(handle.ok()) << handle.error().message();
  ASSERT_TRUE(recorder.WaitForEvent());
  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id(get.id());
  const auto canceled = source_->CancelTask(cancel, options_);
  ASSERT_TRUE(canceled.ok()) << canceled.error().message();
  ASSERT_TRUE(recorder.Wait());
  EXPECT_FALSE(recorder.error().has_value());
  const auto events = recorder.events();
  ASSERT_FALSE(events.empty());
  ASSERT_TRUE(events.front().has_task());
  // SDK subscription snapshots intentionally omit historical artifacts/history.
  auto snapshot = sent.value().task();
  snapshot.clear_artifacts();
  snapshot.clear_history();
  ExpectTaskEqual(snapshot, events.front().task());
  ASSERT_TRUE(events.back().has_status_update());
  EXPECT_EQ(events.back().status_update().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

TEST_P(SharedBindingTest, StreamingResultsAreEquivalent) {
  const auto sent = source_->SendMessage(Message(), options_);
  ASSERT_TRUE(sent.ok()) << sent.error().message();
  auto message = Message(kStreamMessageId);
  message.mutable_message()->set_task_id(sent.value().task().id());
  EventRecorder first;
  EventRecorder second;
  auto first_handle = source_->SendStreamingMessage(message, first, options_);
  ASSERT_TRUE(first_handle.ok()) << first_handle.error().message();
  ASSERT_TRUE(first.Wait());
  auto second_handle = target_->SendStreamingMessage(message, second, options_);
  ASSERT_TRUE(second_handle.ok()) << second_handle.error().message();
  ASSERT_TRUE(second.Wait());
  EXPECT_FALSE(first.error().has_value());
  EXPECT_FALSE(second.error().has_value());
  ExpectEventsEqual(first.events(), second.events());
}

TEST_P(SharedBindingTest, ListTasksProducesEquivalentPages) {
  constexpr std::size_t kPageSize = 10;
  const auto sent = source_->SendMessage(Message(), options_);
  ASSERT_TRUE(sent.ok()) << sent.error().message();
  const a2a::client::ListTasksRequest request{.page_size = kPageSize, .page_token = {}};
  const auto first = source_->ListTasks(request, options_);
  const auto second = target_->ListTasks(request, options_);
  ASSERT_TRUE(first.ok()) << first.error().message();
  ASSERT_TRUE(second.ok()) << second.error().message();
  ASSERT_EQ(first.value().tasks.size(), second.value().tasks.size());
  EXPECT_EQ(first.value().next_page_token, second.value().next_page_token);
  for (std::size_t index = 0; index < first.value().tasks.size(); ++index) {
    SCOPED_TRACE(index);
    ExpectTaskEqual(first.value().tasks[index], second.value().tasks[index]);
  }
}

TEST_P(SharedBindingTest, CursorPagesRoundTripAcrossBindingsInUpdateOrder) {
  const auto older = source_->SendMessage(Message(kCursorFirstMessage), options_);
  ASSERT_TRUE(older.ok()) << older.error().message();
  const auto newer = source_->SendMessage(Message(kCursorSecondMessage), options_);
  ASSERT_TRUE(newer.ok()) << newer.error().message();
  a2a::client::ListTasksRequest request{.page_size = 1, .page_token = {}};
  const auto first = source_->ListTasks(request, options_);
  ASSERT_TRUE(first.ok()) << first.error().message();
  ASSERT_EQ(first.value().tasks.size(), 1U);
  EXPECT_EQ(first.value().tasks.front().id(), newer.value().task().id());
  ASSERT_FALSE(first.value().next_page_token.empty());
  request.page_token = first.value().next_page_token;
  const auto second = target_->ListTasks(request, options_);
  ASSERT_TRUE(second.ok()) << second.error().message();
  ASSERT_EQ(second.value().tasks.size(), 1U);
  EXPECT_EQ(second.value().tasks.front().id(), older.value().task().id());
}

TEST_P(SharedBindingTest, MalformedAndLegacyListCursorsAreRejectedAcrossBindings) {
  for (const auto* token : {kInvalidCursor, kLegacyOffset}) {
    a2a::client::ListTasksRequest request{.page_size = 1, .page_token = token};
    EXPECT_FALSE(source_->ListTasks(request, options_).ok());
    EXPECT_FALSE(target_->ListTasks(request, options_).ok());
  }
}

TEST_P(SharedBindingTest, TerminalTaskSubscriptionUsesEquivalentError) {
  const auto sent = source_->SendMessage(Message(kCompletedMessageId), options_);
  ASSERT_TRUE(sent.ok()) << sent.error().message();
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(sent.value().task().id());
  EventRecorder recorder;
  auto handle = target_->SubscribeTask(get, recorder, options_);
  std::optional<a2a::core::Error> error;
  if (!handle.ok()) {
    error = handle.error();
  } else {
    ASSERT_TRUE(recorder.Wait());
    error = recorder.error();
  }
  ASSERT_TRUE(error.has_value());
  ExpectProtocolFailure(error.value_or(a2a::core::Error::Internal(std::string(kText))), std::get<1>(GetParam()),
                        a2a::core::protocol_codes::kUnsupportedOperation, kGrpcUnimplemented,
                        a2a::core::http::kStatusBadRequest);
}

// BIND-EQUIV-003: SDK adapters must map the same A2A failures.
TEST_P(SharedBindingTest, MissingTaskHasPrescribedBindingError) {
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(kMissingTaskId);
  for (const auto binding : {std::get<0>(GetParam()), std::get<1>(GetParam())}) {
    SCOPED_TRACE(BindingName(binding));
    const auto result = MakeClient(binding)->GetTask(get, options_);
    ASSERT_FALSE(result.ok());
    ExpectProtocolFailure(result.error(), binding, a2a::core::protocol_codes::kTaskNotFound, kGrpcNotFound,
                          a2a::core::http::kStatusNotFound);
  }
}

TEST_P(SharedBindingTest, NegativePushProfileHasPrescribedBindingError) {
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
  request.set_task_id(kMissingTaskId);
  const auto binding = std::get<1>(GetParam());
  const auto result = target_->ListTaskPushNotificationConfigs(request, options_);
  ASSERT_FALSE(result.ok());
  ExpectProtocolFailure(result.error(), binding, a2a::core::protocol_codes::kPushNotificationNotSupported,
                        kGrpcUnimplemented, a2a::core::http::kStatusBadRequest);
}

TEST_P(SharedBindingTest, NegativeStreamingProfileHasPrescribedBindingError) {
  EventRecorder recorder;
  auto handle = target_->SendStreamingMessage(Message(), recorder, options_);
  std::optional<a2a::core::Error> error;
  if (!handle.ok()) {
    error = handle.error();
  } else {
    ASSERT_TRUE(recorder.Wait());
    error = recorder.error();
  }
  ASSERT_TRUE(error.has_value());
  ExpectProtocolFailure(error.value_or(a2a::core::Error::Internal(std::string(kText))), std::get<1>(GetParam()),
                        a2a::core::protocol_codes::kUnsupportedOperation, kGrpcUnimplemented,
                        a2a::core::http::kStatusBadRequest);
}

// BIND-EQUIV-004: a task created through one binding requires the same
// authorization when accessed through any other binding.
TEST_P(SharedBindingTest, AuthenticationRulesSurviveCrossBindingTaskAccess) {
  const auto sent = source_->SendMessage(Message(), options_);
  ASSERT_TRUE(sent.ok()) << sent.error().message();
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(sent.value().task().id());
  CheckUnauthorizedTaskAccess(get);
  const auto allowed = target_->GetTask(get, options_);
  ASSERT_TRUE(allowed.ok()) << allowed.error().message();
  ExpectTaskEqual(sent.value().task(), allowed.value());
}

// VER-SERVER-001..003 against the real SUT with each storage backend.
TEST_P(SharedBindingTest, RequestedVersionsUseSdkNegotiation) {
  constexpr std::string_view kPatchVersion = "1.0.42";
  constexpr std::string_view kUnsupportedVersion = "1.1";
  const auto sent = source_->SendMessage(Message(), options_);
  ASSERT_TRUE(sent.ok()) << sent.error().message();
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(sent.value().task().id());
  auto options = options_;
  options.protocol_version = kPatchVersion;
  const auto patched = target_->GetTask(get, options);
  ASSERT_TRUE(patched.ok()) << patched.error().message();
  ExpectTaskEqual(sent.value().task(), patched.value());
  CheckRejectedVersions(get, kUnsupportedVersion);
}

INSTANTIATE_TEST_SUITE_P(
    AllBindingPairs, SharedBindingTest, testing::Combine(testing::ValuesIn(kBindings), testing::ValuesIn(kBindings)),
    [](const testing::TestParamInfo<BindingPair>& info) {
      return BindingName(std::get<0>(info.param)).append("To").append(BindingName(std::get<1>(info.param)));
    });

}  // namespace
