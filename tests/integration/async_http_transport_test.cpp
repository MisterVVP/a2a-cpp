// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/version.h"

namespace {
using namespace a2a;
using client::ListTasksRequest;
using client::ListTasksResponse;
constexpr auto kWait = std::chrono::seconds(5);
constexpr auto kBaseUrl = "https://async.example.test/a2a";
constexpr auto kTaskId = "async-task";
constexpr auto kConfigId = "async-config";
constexpr auto kRequestId = "async-request";
constexpr auto kRequestHeader = "x-async-metadata";
constexpr auto kHeaderValue = "metadata-value";
constexpr auto kAuth = "Bearer async-credential";
constexpr auto kExtension = "urn:a2a:async-test";
constexpr auto kUnusedSync = "sync requester must not be called";
constexpr auto kRpcPrefix = R"({"jsonrpc":"2.0","id":"async-request","result":)";
constexpr auto kWrongId = R"({"jsonrpc":"2.0","id":"wrong","result":{}})";
constexpr auto kRpcError = R"({"jsonrpc":"2.0","id":"async-request","error":{"code":-32602,"message":"invalid"}})";
constexpr auto kMalformed = R"({"tasks":null})";
constexpr auto kEmpty = "{}";
constexpr auto kMessage = R"({"task":{"id":"async-task"}})";
constexpr auto kTask = R"({"id":"async-task"})";
constexpr auto kTasks = R"({"tasks":[{"id":"async-task"}],"nextPageToken":"next"})";
constexpr auto kConfig = R"({"id":"async-config","taskId":"async-task"})";
constexpr auto kConfigs = R"({"configs":[{"id":"async-config","taskId":"async-task"}]})";
constexpr auto kUnsupportedVersion = "2.0";
constexpr int kInvalidParams = -32602;
constexpr int kOk = 200;
constexpr int kBadGateway = 502;

class AsyncHttpTransportTest : public testing::TestWithParam<client::PreferredTransport> {
 protected:
  void SetUp() override {
    const client::ResolvedInterface resolved{
        .transport = GetParam(), .url = kBaseUrl, .security_requirements = {}, .security_schemes = {}};
    client::HttpRequester sync = [](const auto& request) -> core::Result<client::HttpClientResponse> {
      (void)request;
      return core::Error::Internal(kUnusedSync);
    };
    client::HttpAsyncRequester async =
        [this](client::HttpRequest request,
               core::Completion<client::HttpClientResponse> completion) -> core::Result<core::AsyncHandle> {
      captured_ = std::move(request);
      auto token = network_.Start<client::HttpClientResponse>(std::move(completion), kWait);
      if (!token.ok()) {
        return token.error();
      }
      std::string body = payload_;
      if (GetParam() == client::PreferredTransport::kJsonRpc && !raw_envelope_) {
        body = kRpcPrefix;
        body.append(payload_).push_back('}');
      }
      token.value().Complete(
          client::HttpClientResponse{.status_code = status_,
                                     .headers = {{std::string(core::Version::kHeaderName), version_}},
                                     .body = std::move(body)});
      return token.value().handle();
    };
    if (GetParam() == client::PreferredTransport::kRest) {
      transport_ = std::make_unique<client::HttpJsonTransport>(resolved, std::move(sync), client::HttpStreamRequester{},
                                                               kWait, std::move(async));
    } else {
      transport_ = std::make_unique<client::JsonRpcTransport>(
          resolved, std::move(sync), client::HttpStreamRequester{}, kWait, [] { return std::string(kRequestId); },
          std::move(async));
    }
  }
  template <typename T, typename Start>
  core::Result<T> Await(std::string payload, Start start) {
    payload_ = std::move(payload);
    std::promise<core::Result<T>> result;
    auto future = result.get_future();
    auto operation = start([&](core::Result<T> response) { result.set_value(std::move(response)); });
    EXPECT_TRUE(operation.ok());
    EXPECT_EQ(future.wait_for(kWait), std::future_status::ready);
    auto response = future.get();
    operation.value().Wait();
    return response;
  }
  static client::CallOptions Options() {
    client::CallOptions options;
    options.timeout = kWait;
    options.headers[kRequestHeader] = kHeaderValue;
    options.extensions = {kExtension};
    options.auth_hook = [](client::HeaderMap& headers) {
      headers[std::string(core::http::kAuthorizationHeaderName)] = kAuth;
    };
    return options;
  }
  core::AsyncScope network_;
  std::unique_ptr<client::ClientTransport> transport_;
  client::HttpRequest captured_;
  std::string payload_;
  std::string version_ = std::string(core::Version::kProtocolVersion);
  int status_ = kOk;
  bool raw_envelope_ = false;
};

TEST_P(AsyncHttpTransportTest, SendMessageUsesAsyncRequesterAndDecodes) {
  lf::a2a::v1::SendMessageRequest request;
  auto response = Await<lf::a2a::v1::SendMessageResponse>(kMessage, [&](auto completion) {
    return transport_->SendMessageAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().task().id(), kTaskId);
  EXPECT_EQ(captured_.headers.at(kRequestHeader), kHeaderValue);
  EXPECT_EQ(captured_.headers.at(std::string(core::http::kAuthorizationHeaderName)), kAuth);
  EXPECT_EQ(captured_.headers.at(std::string(core::Extensions::kHeaderName)), kExtension);
  EXPECT_EQ(captured_.timeout, kWait);
}

TEST_P(AsyncHttpTransportTest, GetTaskUsesAsyncRequesterAndDecodes) {
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  auto response = Await<lf::a2a::v1::Task>(
      kTask, [&](auto completion) { return transport_->GetTaskAsync(request, std::move(completion), Options()); });
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kTaskId);
}

TEST_P(AsyncHttpTransportTest, ListTasksUsesAsyncRequesterAndDecodes) {
  ListTasksRequest request;
  auto response = Await<ListTasksResponse>(
      kTasks, [&](auto completion) { return transport_->ListTasksAsync(request, std::move(completion), Options()); });
  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_EQ(response.value().tasks.size(), 1U);
  EXPECT_EQ(response.value().tasks.front().id(), kTaskId);
}

TEST_P(AsyncHttpTransportTest, CancelTaskUsesAsyncRequesterAndDecodes) {
  lf::a2a::v1::CancelTaskRequest request;
  request.set_id(kTaskId);
  auto response = Await<lf::a2a::v1::Task>(
      kTask, [&](auto completion) { return transport_->CancelTaskAsync(request, std::move(completion), Options()); });
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kTaskId);
}

TEST_P(AsyncHttpTransportTest, CreateTaskPushNotificationConfigUsesAsyncRequesterAndDecodes) {
  lf::a2a::v1::TaskPushNotificationConfig request;
  request.set_task_id(kTaskId);
  auto response = Await<lf::a2a::v1::TaskPushNotificationConfig>(kConfig, [&](auto completion) {
    return transport_->CreateTaskPushNotificationConfigAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
}

TEST_P(AsyncHttpTransportTest, GetTaskPushNotificationConfigUsesAsyncRequesterAndDecodes) {
  lf::a2a::v1::GetTaskPushNotificationConfigRequest request;
  request.set_task_id(kTaskId);
  request.set_id(kConfigId);
  auto response = Await<lf::a2a::v1::TaskPushNotificationConfig>(kConfig, [&](auto completion) {
    return transport_->GetTaskPushNotificationConfigAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
}

TEST_P(AsyncHttpTransportTest, ListTaskPushNotificationConfigsUsesAsyncRequesterAndDecodes) {
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
  request.set_task_id(kTaskId);
  auto response = Await<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(kConfigs, [&](auto completion) {
    return transport_->ListTaskPushNotificationConfigsAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
}

TEST_P(AsyncHttpTransportTest, DeleteTaskPushNotificationConfigUsesAsyncRequesterAndDecodes) {
  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request;
  request.set_task_id(kTaskId);
  request.set_id(kConfigId);
  auto response = Await<void>(kEmpty, [&](auto completion) {
    return transport_->DeleteTaskPushNotificationConfigAsync(request, std::move(completion), Options());
  });
  ASSERT_TRUE(response.ok()) << response.error().message();
}

TEST_P(AsyncHttpTransportTest, ListTasksRejectsNullArray) {
  auto response = Await<client::ListTasksResponse>(
      kMalformed, [&](auto completion) { return transport_->ListTasksAsync({}, std::move(completion)); });
  EXPECT_FALSE(response.ok());
}
TEST_P(AsyncHttpTransportTest, ResponseVersionIsValidated) {
  version_ = kUnsupportedVersion;
  auto response = Await<lf::a2a::v1::Task>(kTask, [&](auto completion) {
    lf::a2a::v1::GetTaskRequest request;
    request.set_id(kTaskId);
    return transport_->GetTaskAsync(request, std::move(completion));
  });
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), core::ErrorCode::kUnsupportedVersion);
}
TEST_P(AsyncHttpTransportTest, HttpFailurePreservesStatus) {
  status_ = kBadGateway;
  auto response = Await<client::ListTasksResponse>(
      kEmpty, [&](auto completion) { return transport_->ListTasksAsync({}, std::move(completion)); });
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().http_status(), kBadGateway);
}
TEST_P(AsyncHttpTransportTest, JsonRpcRejectsMismatchedIdsAndMapsRemoteErrors) {
  if (GetParam() != client::PreferredTransport::kJsonRpc) {
    GTEST_SKIP();
  }
  raw_envelope_ = true;
  auto wrong_id = Await<client::ListTasksResponse>(
      kWrongId, [&](auto completion) { return transport_->ListTasksAsync({}, std::move(completion)); });
  EXPECT_FALSE(wrong_id.ok());
  auto remote = Await<client::ListTasksResponse>(
      kRpcError, [&](auto completion) { return transport_->ListTasksAsync({}, std::move(completion)); });
  ASSERT_FALSE(remote.ok());
  EXPECT_EQ(remote.error().protocol_code(), std::to_string(kInvalidParams));
}
INSTANTIATE_TEST_SUITE_P(HttpBindings, AsyncHttpTransportTest,
                         testing::Values(client::PreferredTransport::kRest, client::PreferredTransport::kJsonRpc));
}  // namespace
