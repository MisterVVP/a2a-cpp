// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <google/protobuf/struct.pb.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server_builder.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>

#include "a2a/client/grpc_transport.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protojson.h"
#include "a2a/server/grpc_server_transport.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/rest_server_transport.h"
#include "example_support.h"
#include "support/security/fixtures.h"

namespace {
using Binding = a2a::client::PreferredTransport;
using namespace a2a::tests::security;
constexpr char kCertificateEnv[] = "A2A_TCK_TLS_CERT_FILE";
constexpr char kPrivateKeyEnv[] = "A2A_TCK_TLS_KEY_FILE";
constexpr char kCaEnv[] = "A2A_SECURITY_CA_FILE";
constexpr char kTlsHostEnv[] = "A2A_SECURITY_TLS_HOST";
constexpr char kModeEnv[] = "A2A_SECURITY_MODE";
constexpr std::string_view kTrustedMode = "trusted";
constexpr std::string_view kPlaintextMode = "plaintext";
constexpr std::string_view kHostSeparator = ":";
constexpr std::string_view kTlsTestScope = "TLS fixture assertions apply to native gRPC; HTTP adapters are in-process";
constexpr std::string_view kRestBindingName = "HttpJson";
constexpr std::string_view kRpcBindingName = "JsonRpc";
constexpr std::string_view kGrpcBindingName = "Grpc";
constexpr std::array kBindings{Binding::kRest, Binding::kJsonRpc, Binding::kGrpc};
constexpr std::string_view kBindAddress = "127.0.0.1:0";
constexpr std::string_view kHostPrefix = "127.0.0.1:";
constexpr std::string_view kRestUrl = "http://agent.local/a2a";
constexpr std::string_view kRpcUrl = "http://agent.local/rpc";
constexpr std::string_view kRpcPath = "/rpc";
constexpr auto kCallTimeout = std::chrono::seconds(2);
constexpr std::string_view kRestPath = "/a2a";
constexpr std::string_view kTaskId = "sdk-transport-owned-task";
constexpr std::string_view kOtherTaskId = "sdk-transport-other-task";
constexpr std::string_view kMissingTaskId = "sdk-transport-missing-task";
constexpr std::string_view kErrorField = "error";
constexpr std::string_view kMessageField = "message";
constexpr std::string_view kAlice = "alice";
constexpr std::string_view kBob = "bob";
constexpr std::string_view kBearerKey = "bearer_token";
constexpr int kUnauthorized = 401;
constexpr int kGrpcUnauthenticated = 16;
constexpr int kNotFound = 404;
constexpr int kGrpcNotFound = 5;
constexpr std::size_t kTaskCount = 1;

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

std::string Environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
}
std::string ReadFile(const std::string& path) {
  std::ifstream file(path);
  return {std::istreambuf_iterator<char>(file), {}};
}
std::shared_ptr<grpc::ServerCredentials> ServerCredentials() {
  const auto certificate = Environment(kCertificateEnv);
  if (certificate.empty()) {
    return grpc::InsecureServerCredentials();
  }
  grpc::SslServerCredentialsOptions options;
  options.pem_key_cert_pairs.push_back({ReadFile(Environment(kPrivateKeyEnv)), ReadFile(certificate)});
  return grpc::SslServerCredentials(options);
}
std::shared_ptr<grpc::ChannelCredentials> ChannelCredentials() {
  const auto ca = Environment(kCaEnv);
  if (ca.empty()) {
    return grpc::InsecureChannelCredentials();
  }
  grpc::SslCredentialsOptions options;
  options.pem_root_certs = ReadFile(ca);
  return grpc::SslCredentials(options);
}

class Policy final : public a2a::server::SecurityPolicy {
 public:
  [[nodiscard]] a2a::core::Result<std::string> Authenticate(const a2a::server::RequestContext& context) const override {
    const auto token = context.auth_metadata.find(std::string(kBearerKey));
    if (token == context.auth_metadata.end()) {
      return a2a::core::protocol_errors::Unauthenticated();
    }
    if (token->second == kAliceToken) {
      return std::string(kAlice);
    }
    if (token->second == kBobToken) {
      return std::string(kBob);
    }
    return a2a::core::protocol_errors::Unauthenticated();
  }
  [[nodiscard]] bool AuthorizeOperation(const a2a::server::DispatchRequest& /*request*/,
                                        const a2a::server::RequestContext& /*context*/) const override {
    return true;
  }
  [[nodiscard]] bool CanAccessTask(const lf::a2a::v1::Task& task,
                                   const a2a::server::RequestContext& context) const override {
    return task.context_id() == context.principal;
  }
};

class ResponseExecutor final : public a2a::examples::ExampleExecutor {
 public:
  using ExampleExecutor::ExampleExecutor;
  a2a::core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                               a2a::server::RequestContext& context) override {
    return task_response.has_value() ? a2a::core::Result<lf::a2a::v1::Task>(*task_response)
                                     : ExampleExecutor::GetTask(request, context);
  }
  a2a::core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                  a2a::server::RequestContext& context) override {
    return task_response.has_value() ? a2a::core::Result<lf::a2a::v1::Task>(*task_response)
                                     : ExampleExecutor::CancelTask(request, context);
  }
  std::optional<lf::a2a::v1::Task> task_response;
};

class SecurityTransportIntegrationTest : public testing::TestWithParam<Binding> {
 protected:
  void SetUp() override {
    SaveTask(kTaskId, kAlice);
    SaveTask(kOtherTaskId, kBob);
    dispatcher_.SetSecurityPolicy(std::make_shared<Policy>(), &store_);
    grpc::ServerBuilder builder;
    int port = 0;
    builder.AddListeningPort(std::string(kBindAddress), ServerCredentials(), &port);
    builder.RegisterService(&grpc_adapter_);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    const auto tls_host = Environment(kTlsHostEnv);
    const auto grpc_url = tls_host.empty() ? std::string(kHostPrefix).append(std::to_string(port))
                                           : std::string(tls_host).append(kHostSeparator).append(std::to_string(port));
    a2a::client::ResolvedInterface iface{
        .transport = GetParam(), .url = {}, .security_requirements = {}, .security_schemes = {}};
    std::unique_ptr<a2a::client::ClientTransport> transport;
    if (GetParam() == Binding::kGrpc) {
      iface.url = grpc_url;
      transport =
          std::make_unique<a2a::client::GrpcTransport>(iface, grpc::CreateChannel(grpc_url, ChannelCredentials()));
    } else {
      iface.url = GetParam() == Binding::kRest ? kRestUrl : kRpcUrl;
      auto requester = [this](const a2a::client::HttpRequest& request) { return Handle(request); };
      if (GetParam() == Binding::kRest) {
        transport = std::make_unique<a2a::client::HttpJsonTransport>(iface, requester);
      } else {
        transport = std::make_unique<a2a::client::JsonRpcTransport>(iface, requester);
      }
    }
    client_ = std::make_unique<a2a::client::A2AClient>(std::move(transport));
  }
  void TearDown() override {
    client_.reset();
    if (server_) {
      server_->Shutdown();
    }
  }
  void SaveTask(std::string_view id, std::string_view owner) {
    lf::a2a::v1::Task task;
    task.set_id(std::string(id));
    task.set_context_id(std::string(owner));
    task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
    ASSERT_TRUE(store_.CreateOrUpdate(task).ok());
  }
  a2a::core::Result<a2a::client::HttpClientResponse> Handle(const a2a::client::HttpRequest& request) {
    a2a::server::HttpServerRequest incoming{.method = request.method,
                                            .target = a2a::core::ExtractTargetPath(request.url),
                                            .headers = request.headers,
                                            .body = request.body,
                                            .remote_address = {}};
    auto response = GetParam() == Binding::kRest ? rest_adapter_.Handle(incoming) : rpc_adapter_.Handle(incoming);
    if (!response.ok()) {
      return response.error();
    }
    last_http_body_ = response.value().body;
    return a2a::client::HttpClientResponse{.status_code = response.value().status_code,
                                           .headers = response.value().headers,
                                           .body = response.value().body};
  }
  static a2a::client::CallOptions Options(std::string_view token) {
    a2a::client::CallOptions options;
    options.timeout = kCallTimeout;
    options.credential_provider = std::make_shared<a2a::client::BearerTokenCredentialProvider>(std::string(token));
    return options;
  }
  static void ExpectUnauthenticated(const a2a::core::Error& error) {
    if (GetParam() == Binding::kGrpc) {
      EXPECT_EQ(error.protocol_code(), std::to_string(kGrpcUnauthenticated));
    } else {
      EXPECT_EQ(error.http_status(), kUnauthorized);
    }
  }
  static void ExpectHidden(const a2a::core::Error& error) {
    if (GetParam() == Binding::kGrpc) {
      EXPECT_EQ(error.protocol_code(), std::to_string(kGrpcNotFound));
    } else if (GetParam() == Binding::kRest) {
      EXPECT_EQ(error.http_status(), kNotFound);
    } else {
      EXPECT_EQ(error.protocol_code(), a2a::core::protocol_codes::kTaskNotFound);
    }
  }
  static void ExpectHiddenTask(const a2a::core::Result<lf::a2a::v1::Task>& result) {
    ASSERT_FALSE(result.ok());
    ExpectHidden(result.error());
  }
  std::string ServerErrorMessage(const a2a::core::Error& error) const {
    if (GetParam() == Binding::kGrpc) {
      return std::string(error.message());
    }
    google::protobuf::Struct envelope;
    const auto parsed = a2a::core::JsonToMessage(last_http_body_, &envelope);
    EXPECT_TRUE(parsed.ok());
    if (!parsed.ok()) {
      return {};
    }
    return envelope.fields()
        .at(std::string(kErrorField))
        .struct_value()
        .fields()
        .at(std::string(kMessageField))
        .string_value();
  }
  std::string last_http_body_;
  a2a::server::InMemoryTaskStore store_;
  ResponseExecutor executor_{
      {.task_store = &store_, .push_store = nullptr, .push_delivery = nullptr, .task_id_generator = {}}};
  a2a::server::Dispatcher dispatcher_{&executor_};
  a2a::server::RestServerTransport rest_adapter_{&dispatcher_,
                                                 {},
                                                 {.rest_api_base_path = std::string(kRestPath),
                                                  .require_version_header = true,
                                                  .include_legacy_transport_fields = false,
                                                  .agent_card_cache_settings = std::nullopt,
                                                  .required_extensions = {}}};
  a2a::server::JsonRpcServerTransport rpc_adapter_{&dispatcher_, {.rpc_path = std::string(kRpcPath)}};
  a2a::server::GrpcServerTransport grpc_adapter_{&dispatcher_};
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<a2a::client::A2AClient> client_;
};

TEST_P(SecurityTransportIntegrationTest, AuthenticationAndHiddenResourcesUseBindingErrors) {
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(std::string(kTaskId));
  auto missing = client_->GetTask(get, {});
  ASSERT_FALSE(missing.ok());
  ExpectUnauthenticated(missing.error());
  EXPECT_TRUE(client_->GetTask(get, Options(kAliceToken)).ok());
  auto hidden = client_->GetTask(get, Options(kBobToken));
  ASSERT_FALSE(hidden.ok());
  ExpectHidden(hidden.error());
  const auto hidden_message = ServerErrorMessage(hidden.error());
  get.set_id(std::string(kMissingTaskId));
  auto absent = client_->GetTask(get, Options(kBobToken));
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(hidden_message, ServerErrorMessage(absent.error()));
}
TEST_P(SecurityTransportIntegrationTest, ListsIncludeOnlyAuthorizedResources) {
  auto listed = client_->ListTasks({}, Options(kBobToken));
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value().tasks.size(), kTaskCount);
  EXPECT_EQ(listed.value().tasks.front().id(), kOtherTaskId);
  EXPECT_TRUE(listed.value().next_page_token.empty());
}
TEST_P(SecurityTransportIntegrationTest, GetAndCancelRejectForeignExecutorResponses) {
  auto foreign = store_.Get(kOtherTaskId);
  ASSERT_TRUE(foreign.ok());
  executor_.task_response = std::move(foreign.value());
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(std::string(kTaskId));
  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id(std::string(kTaskId));
  ExpectHiddenTask(client_->GetTask(get, Options(kAliceToken)));
  ExpectHiddenTask(client_->CancelTask(cancel, Options(kAliceToken)));
}
TEST_P(SecurityTransportIntegrationTest, TlsCertificateValidationWithoutSut) {
  if (GetParam() != Binding::kGrpc) {
    GTEST_SKIP() << kTlsTestScope;
  }
  const auto mode = Environment(kModeEnv);
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(std::string(kTaskId));
  const auto result = client_->GetTask(get, Options(kAliceToken));
  if (mode.empty() || mode == kTrustedMode || mode == kPlaintextMode) {
    EXPECT_TRUE(result.ok());
  } else {
    EXPECT_FALSE(result.ok());
  }
}
INSTANTIATE_TEST_SUITE_P(AllBindings, SecurityTransportIntegrationTest, testing::ValuesIn(kBindings),
                         [](const testing::TestParamInfo<Binding>& info) { return BindingName(info.param); });
}  // namespace
