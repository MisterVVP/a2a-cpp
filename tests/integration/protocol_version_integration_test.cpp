// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
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
#include "a2a/core/http_utils.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/protojson.h"
#include "a2a/core/string_utils.h"
#include "a2a/core/url_utils.h"
#include "a2a/core/version.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "example_support.h"

namespace {

using Binding = a2a::client::PreferredTransport;
using Operation = a2a::server::DispatcherOperation;
constexpr std::string_view kRestUrl = "http://agent.local/a2a";
constexpr std::string_view kRpcUrl = "http://agent.local/rpc";
constexpr std::string_view kRestPath = "/a2a";
constexpr std::string_view kRpcPath = "/rpc";
constexpr std::string_view kBindAddress = "127.0.0.1:0";
constexpr std::string_view kHostPrefix = "127.0.0.1:";
constexpr std::string_view kTaskId = "version-task";
constexpr std::string_view kMessageId = "version-message";
constexpr std::string_view kText = "version semantics";
constexpr std::string_view kPushId = "version-push";
constexpr std::string_view kPushUrl = "https://example.invalid/version-push";
constexpr std::string_view kPatchVersion = "1.0.42";
constexpr std::string_view kUnsupportedVersion = "1.1.0";
constexpr std::string_view kLowercaseVersionHeader = "a2a-version";
constexpr std::string_view kConflictingVersion = "9.9";
constexpr std::string_view kGetTaskRpcBody =
    R"({"jsonrpc":"2.0","id":"version","method":"GetTask","params":{"id":"version-task"}})";
constexpr auto kDeadline = std::chrono::seconds(5);
constexpr std::size_t kOperationCount = 10;
constexpr auto kBindingLabels = std::to_array<std::string_view>({"HttpJson", "JsonRpc", "Grpc"});
constexpr auto kOperationLabels = std::to_array<std::string_view>(
    {"SendMessage", "GetTask", "ListTasks", "CancelTask", "CreateTaskPushNotificationConfig",
     "GetTaskPushNotificationConfig", "ListTaskPushNotificationConfigs", "DeleteTaskPushNotificationConfig",
     "SendStreamingMessage", "SubscribeTask"});
constexpr auto kOperations = std::to_array<Operation>(
    {Operation::kSendMessage, Operation::kGetTask, Operation::kListTasks, Operation::kCancelTask,
     Operation::kCreateTaskPushNotificationConfig, Operation::kGetTaskPushNotificationConfig,
     Operation::kListTaskPushNotificationConfigs, Operation::kDeleteTaskPushNotificationConfig,
     Operation::kSendStreamingMessage, Operation::kSubscribeTask});

// Independent of the SUT: this SDK interceptor records real parsed HTTP headers
// or gRPC metadata and the version selected by the SDK server before dispatch.
class VersionRecorder final : public a2a::server::ServerInterceptor {
 public:
  a2a::core::Result<void> BeforeDispatch(const a2a::server::DispatchRequest& request,
                                         a2a::server::RequestContext& context) override {
    std::lock_guard lock(mutex_);
    operations_.push_back(request.operation);
    requested_.emplace_back(a2a::core::http::FindHeaderValue(context.client_headers, a2a::core::Version::kHeaderName)
                                .value_or(std::string_view{}));
    selected_.push_back(context.protocol_version);
    // A deterministic SDK error makes every operation safe and finite. Its
    // presence distinguishes supported-version dispatch from version rejection.
    return a2a::core::protocol_errors::UnsupportedOperation();
  }
  void ExpectAll(std::string_view requested) const {
    std::lock_guard lock(mutex_);
    ASSERT_EQ(operations_.size(), kOperationCount);
    for (std::size_t index = 0; index < kOperations.size(); ++index) {
      SCOPED_TRACE(kOperationLabels[index]);
      EXPECT_EQ(operations_[index], kOperations[index]);
      EXPECT_EQ(requested_[index], requested);
      EXPECT_EQ(selected_[index], a2a::core::Version::kProtocolVersion);
    }
  }
  std::size_t size() const {
    std::lock_guard lock(mutex_);
    return operations_.size();
  }
  std::string selected() const {
    std::lock_guard lock(mutex_);
    return selected_.empty() ? std::string{} : selected_.back();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Operation> operations_;
  std::vector<std::string> requested_;
  std::vector<std::string> selected_;
};

class ErrorObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& event) override { (void)event; }
  void OnError(const a2a::core::Error& error) override {
    (void)error;
    Finish();
  }
  void OnCompleted() override { Finish(); }
  bool Wait() {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, kDeadline, [this] { return finished_; });
  }

 private:
  void Finish() {
    std::lock_guard lock(mutex_);
    finished_ = true;
    ready_.notify_all();
  }
  std::mutex mutex_;
  std::condition_variable ready_;
  bool finished_ = false;
};

class ProtocolVersionIntegrationTest : public testing::TestWithParam<Binding> {
 protected:
  void SetUp() override {
    recorder_ = std::make_shared<VersionRecorder>();
    dispatcher_.AddInterceptor(recorder_);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(std::string(kBindAddress), grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(&grpc_adapter_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    grpc_url_ = std::string(kHostPrefix).append(std::to_string(port_));
    channel_ = grpc::CreateChannel(grpc_url_, grpc::InsecureChannelCredentials());
    a2a::client::ResolvedInterface iface{
        .transport = GetParam(), .url = {}, .security_requirements = {}, .security_schemes = {}};
    std::unique_ptr<a2a::client::ClientTransport> transport;
    if (GetParam() == Binding::kGrpc) {
      iface.url = grpc_url_;
      transport = std::make_unique<a2a::client::GrpcTransport>(iface, channel_);
    } else {
      iface.url = GetParam() == Binding::kRest ? kRestUrl : kRpcUrl;
      const auto requester = [this](const a2a::client::HttpRequest& request) { return Handle(request); };
      const auto streaming =
          [this](const a2a::client::HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& metadata,
                 const a2a::client::HttpStreamChunkHandler& chunk,
                 const a2a::client::StreamCancelled& cancelled) -> a2a::core::Result<a2a::client::HttpClientResponse> {
        (void)chunk;
        (void)cancelled;
        auto response = Handle(request);
        if (!response.ok()) {
          return response.error();
        }
        const auto validated = metadata(response.value());
        if (!validated.ok()) {
          return validated.error();
        }
        return response;
      };
      if (GetParam() == Binding::kRest) {
        transport = std::make_unique<a2a::client::HttpJsonTransport>(iface, requester, streaming);
      } else {
        transport = std::make_unique<a2a::client::JsonRpcTransport>(iface, requester, streaming);
      }
    }
    client_ = std::make_unique<a2a::client::A2AClient>(std::move(transport));
  }
  void TearDown() override {
    client_.reset();
    if (server_ != nullptr) {
      server_->Shutdown();
    }
  }
  a2a::core::Result<a2a::client::HttpClientResponse> Handle(const a2a::client::HttpRequest& request) const {
    const auto aliases = std::ranges::count_if(request.headers, [](const auto& header) {
      return a2a::core::strings::EqualsAsciiCaseInsensitive(header.first, a2a::core::Version::kHeaderName);
    });
    EXPECT_EQ(aliases, 1);
    a2a::server::HttpServerRequest incoming{.method = request.method,
                                            .target = a2a::core::ExtractTargetPath(request.url),
                                            .headers = request.headers,
                                            .body = request.body,
                                            .remote_address = {}};
    const auto response = GetParam() == Binding::kRest ? rest_adapter_.Handle(incoming) : rpc_adapter_.Handle(incoming);
    if (!response.ok()) {
      return response.error();
    }
    return a2a::client::HttpClientResponse{.status_code = response.value().status_code,
                                           .headers = response.value().headers,
                                           .body = response.value().body};
  }
  static lf::a2a::v1::GetTaskRequest GetRequest() {
    lf::a2a::v1::GetTaskRequest get;
    get.set_id(std::string(kTaskId));
    return get;
  }
  void ExerciseAllOperations(const a2a::client::CallOptions& options) {
    lf::a2a::v1::SendMessageRequest send;
    send.mutable_message()->set_message_id(std::string(kMessageId));
    send.mutable_message()->set_role(lf::a2a::v1::ROLE_USER);
    send.mutable_message()->add_parts()->set_text(std::string(kText));
    EXPECT_FALSE(client_->SendMessage(send, options).ok());
    EXPECT_FALSE(client_->GetTask(GetRequest(), options).ok());
    EXPECT_FALSE(client_->ListTasks({}, options).ok());
    lf::a2a::v1::CancelTaskRequest cancel;
    cancel.set_id(std::string(kTaskId));
    EXPECT_FALSE(client_->CancelTask(cancel, options).ok());
    ExercisePush(options);
    ExerciseStreams(send, options);
  }
  void ExercisePush(const a2a::client::CallOptions& options) {
    lf::a2a::v1::TaskPushNotificationConfig create;
    create.set_task_id(std::string(kTaskId));
    create.set_id(std::string(kPushId));
    create.set_url(std::string(kPushUrl));
    EXPECT_FALSE(client_->CreateTaskPushNotificationConfig(create, options).ok());
    lf::a2a::v1::GetTaskPushNotificationConfigRequest get;
    get.set_task_id(std::string(kTaskId));
    get.set_id(std::string(kPushId));
    EXPECT_FALSE(client_->GetTaskPushNotificationConfig(get, options).ok());
    lf::a2a::v1::ListTaskPushNotificationConfigsRequest list;
    list.set_task_id(std::string(kTaskId));
    EXPECT_FALSE(client_->ListTaskPushNotificationConfigs(list, options).ok());
    lf::a2a::v1::DeleteTaskPushNotificationConfigRequest remove;
    remove.set_task_id(std::string(kTaskId));
    remove.set_id(std::string(kPushId));
    EXPECT_FALSE(client_->DeleteTaskPushNotificationConfig(remove, options).ok());
  }
  void ExerciseStreams(const lf::a2a::v1::SendMessageRequest& send, const a2a::client::CallOptions& options) {
    ErrorObserver streaming;
    auto stream = client_->SendStreamingMessage(send, streaming, options);
    if (stream.ok()) {
      ASSERT_TRUE(streaming.Wait());
    }
    ErrorObserver subscribing;
    auto subscription = client_->SubscribeTask(GetRequest(), subscribing, options);
    if (subscription.ok()) {
      ASSERT_TRUE(subscribing.Wait());
    }
  }

  void CheckGrpcWirePatch() const {
    auto stub = lf::a2a::v1::A2AService::NewStub(channel_);
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + kDeadline);
    context.AddMetadata(std::string(kLowercaseVersionHeader), std::string(kPatchVersion));
    lf::a2a::v1::Task task;
    const auto status = stub->GetTask(&context, GetRequest(), &task);
    EXPECT_EQ(status.error_code(), grpc::StatusCode::UNIMPLEMENTED);
    CheckGrpcProtocolCode(context);
  }
  static void CheckGrpcProtocolCode(const grpc::ClientContext& context) {
    const auto& metadata = context.GetServerTrailingMetadata();
    const auto protocol = metadata.find(std::string(a2a::server::GrpcServerTransport::kProtocolCodeMetadataKey));
    ASSERT_NE(protocol, metadata.end());
    EXPECT_EQ(std::string(protocol->second.data(), protocol->second.size()),
              a2a::core::protocol_codes::kUnsupportedOperation);
  }
  void CheckHttpWirePatch() const {
    std::string url = std::string(kRestUrl).append("/tasks/").append(kTaskId);
    if (GetParam() == Binding::kJsonRpc) {
      url = kRpcUrl;
    }
    const auto response =
        Handle({.method = GetParam() == Binding::kRest ? "GET" : "POST",
                .url = url,
                .headers = {{std::string(a2a::core::Version::kHeaderName), std::string(kPatchVersion)},
                            {std::string(a2a::core::http::kContentTypeHeaderName),
                             std::string(a2a::core::http::kContentTypeApplicationJson)}},
                .body = GetParam() == Binding::kRest ? std::string{} : std::string(kGetTaskRpcBody)});
    ASSERT_TRUE(response.ok()) << response.error().message();
  }

  static void ExpectVersionFailure(const a2a::core::Error& error) {
    switch (GetParam()) {
      case Binding::kGrpc:
        EXPECT_EQ(error.protocol_code(), std::to_string(static_cast<int>(grpc::StatusCode::UNIMPLEMENTED)));
        break;
      case Binding::kJsonRpc:
        EXPECT_EQ(error.protocol_code(), a2a::core::protocol_codes::kVersionNotSupported);
        EXPECT_EQ(error.http_status(), a2a::core::http::kStatusOk);
        break;
      case Binding::kRest:
        EXPECT_EQ(error.http_status(), a2a::core::http::kStatusBadRequest);
        break;
    }
  }

  a2a::examples::ExampleExecutor executor_;
  a2a::server::Dispatcher dispatcher_{&executor_};
  a2a::server::RestServerTransport rest_adapter_{&dispatcher_,
                                                 a2a::examples::BuildRestAgentCard(kMessageId, kRestUrl),
                                                 {.rest_api_base_path = std::string(kRestPath),
                                                  .include_legacy_transport_fields = false,
                                                  .agent_card_cache_settings = std::nullopt}};
  a2a::server::JsonRpcServerTransport rpc_adapter_{&dispatcher_, {.rpc_path = std::string(kRpcPath)}};
  a2a::server::GrpcServerTransport grpc_adapter_{&dispatcher_};
  std::shared_ptr<VersionRecorder> recorder_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<a2a::client::A2AClient> client_;
  std::string grpc_url_;
  int port_ = 0;
};

// VER-CLIENT-001/002: all eight unary/push methods plus streaming and subscription.
TEST_P(ProtocolVersionIntegrationTest, EveryClientOperationSendsDefaultVersion) {
  ExerciseAllOperations({});
  recorder_->ExpectAll(a2a::core::Version::kProtocolVersion);
}

TEST_P(ProtocolVersionIntegrationTest, RequestedPatchIsNormalizedAndHeaderAliasesAreRemoved) {
  a2a::client::CallOptions options;
  options.protocol_version = kPatchVersion;
  options.headers[std::string(kLowercaseVersionHeader)] = kConflictingVersion;
  options.auth_hook = [](a2a::client::HeaderMap& headers) {
    headers[std::string(a2a::core::Version::kHeaderName)] = kConflictingVersion;
  };
  ExerciseAllOperations(options);
  recorder_->ExpectAll(a2a::core::Version::kProtocolVersion);
}

// VER-SERVER-002/003: unsupported minor/empty fallback must never reach dispatch.
TEST_P(ProtocolVersionIntegrationTest, UnsupportedMinorAndEmptyFallbackNeverDispatch) {
  for (const auto version : {kUnsupportedVersion, std::string_view{}}) {
    SCOPED_TRACE(version);
    a2a::client::CallOptions options;
    options.protocol_version = version;
    ExerciseAllOperations(options);
    EXPECT_EQ(recorder_->size(), 0);
  }
}

TEST_P(ProtocolVersionIntegrationTest, UnsupportedVersionUsesPrescribedBindingCode) {
  a2a::client::CallOptions options;
  options.protocol_version = kUnsupportedVersion;
  const auto rejected = client_->GetTask(GetRequest(), options);
  ASSERT_FALSE(rejected.ok());
  ExpectVersionFailure(rejected.error());
  EXPECT_EQ(recorder_->size(), 0);
}

TEST_P(ProtocolVersionIntegrationTest, MalformedClientVersionIsRejectedBeforeSending) {
  constexpr std::string_view kMalformedVersion = "1.0.bad";
  a2a::client::CallOptions options;
  options.protocol_version = kMalformedVersion;
  const auto rejected = client_->GetTask(GetRequest(), options);
  ASSERT_FALSE(rejected.ok());
  EXPECT_EQ(rejected.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_EQ(recorder_->size(), 0);
}

// VER-SERVER-001/002: send an actual patch on the wire (without a client
// normalizing it) and verify the SDK selects the supported 1.0 semantics.
TEST_P(ProtocolVersionIntegrationTest, ServerIgnoresWirePatchBeforeDispatch) {
  if (GetParam() == Binding::kGrpc) {
    CheckGrpcWirePatch();
  } else {
    CheckHttpWirePatch();
  }
  EXPECT_EQ(recorder_->size(), 1);
  EXPECT_EQ(recorder_->selected(), a2a::core::Version::kProtocolVersion);
}

INSTANTIATE_TEST_SUITE_P(AllBindings, ProtocolVersionIntegrationTest,
                         testing::Values(Binding::kRest, Binding::kJsonRpc, Binding::kGrpc),
                         [](const testing::TestParamInfo<Binding>& info) {
                           return std::string(kBindingLabels[static_cast<std::size_t>(info.param)]);
                         });

}  // namespace
