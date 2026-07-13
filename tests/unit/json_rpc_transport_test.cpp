// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/json_rpc_transport.h"

#include <google/protobuf/struct.pb.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "a2a/client/auth.h"
#include "a2a/client/client.h"
#include "a2a/core/error.h"
#include "a2a/core/protojson.h"

namespace {

using a2a::client::A2AClient;
using a2a::client::CallOptions;
using a2a::client::HttpClientResponse;
using a2a::client::HttpRequest;
using a2a::client::JsonRpcTransport;
using a2a::client::PreferredTransport;
using a2a::client::ResolvedInterface;
using a2a::core::ErrorCode;

constexpr int kHttpOk = 200;
constexpr int kHttpServerError = 500;
constexpr int kHttpBadGateway = 502;
constexpr std::chrono::milliseconds kCustomTimeout{1200};

ResolvedInterface MakeResolvedJsonRpc() {
  ResolvedInterface resolved;
  resolved.transport = PreferredTransport::kJsonRpc;
  resolved.url = "https://agent.example.com/rpc";
  return resolved;
}

std::string SuccessGetTaskEnvelope(std::string_view request_id) {
  return std::string(R"({"jsonrpc":"2.0","id":")") + std::string(request_id) + R"(","result":{"id":"t-1"}})";
}

a2a::core::Result<google::protobuf::Struct> ParseJsonStruct(const std::string& body) {
  google::protobuf::Struct value;
  const auto parsed = a2a::core::JsonToMessage(body, &value);
  if (!parsed.ok()) {
    return parsed.error();
  }
  return value;
}

TEST(JsonRpcTransportUnitTest, UsesPostToResolvedJsonRpcUrl) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = SuccessGetTaskEnvelope("req-123")};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(captured.method, "POST");
  EXPECT_EQ(captured.url, "https://agent.example.com/rpc");
}

TEST(JsonRpcTransportUnitTest, RespectsTimeoutOverrideFromCallOptions) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        EXPECT_EQ(request.timeout, kCustomTimeout);
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = SuccessGetTaskEnvelope("req-123")};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  CallOptions options;
  options.timeout = kCustomTimeout;

  const auto response = client.GetTask(request, options);
  ASSERT_TRUE(response.ok()) << response.error().message();
}

TEST(JsonRpcTransportUnitTest, SerializesExpectedEnvelopeFields) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = SuccessGetTaskEnvelope("req-123")};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_TRUE(response.ok()) << response.error().message();

  const auto envelope = ParseJsonStruct(captured.body);
  ASSERT_TRUE(envelope.ok()) << envelope.error().message();
  EXPECT_EQ(envelope.value().fields().at("jsonrpc").string_value(), "2.0");
  EXPECT_EQ(envelope.value().fields().at("id").string_value(), "req-123");
  EXPECT_EQ(envelope.value().fields().at("method").string_value(), "a2a.getTask");
  EXPECT_TRUE(envelope.value().fields().contains("params"));
}

TEST(JsonRpcTransportUnitTest, ResponseIdMismatchReturnsRemoteProtocolError) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = R"({"jsonrpc":"2.0","id":"other","result":{"id":"t-1"}})"};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "expected-id"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kRemoteProtocol);
  EXPECT_EQ(response.error().transport().value_or(""), "jsonrpc");
}

TEST(JsonRpcTransportUnitTest, MalformedEnvelopeReturnsSerializationError) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = "{not-json"};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kSerialization);
  EXPECT_EQ(response.error().transport().value_or(""), "jsonrpc");
}

TEST(JsonRpcTransportUnitTest, InjectsApiKeyHeaderViaCredentialProvider) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = SuccessGetTaskEnvelope("req-123")};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  CallOptions options;
  options.credential_provider = std::make_shared<a2a::client::ApiKeyCredentialProvider>("secret-key", "X-API-Key");

  const auto response = client.GetTask(request, options);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(captured.headers["X-API-Key"], "secret-key");
}

TEST(JsonRpcTransportUnitTest, InjectsBearerTokenAndMtlsConfiguration) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = SuccessGetTaskEnvelope("req-123")};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  CallOptions options;
  options.credential_provider = std::make_shared<a2a::client::BearerTokenCredentialProvider>("token-123");
  options.mtls = a2a::client::MtlsConfig{.client_certificate_pem = "cert",
                                         .client_private_key_pem = "key",
                                         .trusted_ca_pem = "",
                                         .server_name_override = ""};

  const auto response = client.GetTask(request, options);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(captured.headers["Authorization"], "Bearer token-123");
  ASSERT_TRUE(captured.mtls.has_value());
  const a2a::client::MtlsConfig mtls = captured.mtls.value_or(a2a::client::MtlsConfig{});
  EXPECT_EQ(mtls.client_certificate_pem, "cert");
}

TEST(JsonRpcTransportUnitTest, InjectsCustomHeadersViaCredentialProvider) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = SuccessGetTaskEnvelope("req-123")};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  CallOptions options;
  options.credential_provider =
      std::make_shared<a2a::client::CustomHeaderCredentialProvider>(a2a::client::HeaderMap{{"X-Custom-Auth", "abc"}});

  const auto response = client.GetTask(request, options);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(captured.headers["X-Custom-Auth"], "abc");
}

TEST(JsonRpcTransportUnitTest, ListTasksUsesListTasksMethodAndParsesResponse) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = R"({"jsonrpc":"2.0","id":"req-123","result":{"tasks":[{"id":"task-1"}]}})"};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  const auto response = client.ListTasks({.page_size = 20, .page_token = "cursor"});
  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_EQ(response.value().tasks.size(), 1U);
  EXPECT_EQ(response.value().tasks[0].id(), "task-1");

  const auto envelope = ParseJsonStruct(captured.body);
  ASSERT_TRUE(envelope.ok()) << envelope.error().message();
  EXPECT_EQ(envelope.value().fields().at("method").string_value(), "a2a.listTasks");
}

TEST(JsonRpcTransportUnitTest, RejectsNonSuccessHttpStatusEvenWithResultEnvelope) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpServerError,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = R"({"jsonrpc":"2.0","id":"req-123","result":{"id":"t-1"}})"};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kRemoteProtocol);
}

TEST(JsonRpcTransportUnitTest, ReturnsUnsupportedVersionOnInvalidA2AVersionHeader) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"a2a-version", "999.0"}},
                                  .body = R"({"jsonrpc":"2.0","id":"req-123","result":{"id":"t-1"}})"};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kUnsupportedVersion);
}

TEST(JsonRpcTransportUnitTest, ParsesJsonRpcErrorObjectProtocolCodeAndMessage) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{
            .status_code = kHttpBadGateway,
            .headers = {{"A2A-Version", "1.0"}},
            .body = R"({"jsonrpc":"2.0","id":"req-123","error":{"code":-32601,"message":"missing method"}})"};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kRemoteProtocol);
  EXPECT_EQ(response.error().protocol_code().value_or(""), "-32601");
}

TEST(JsonRpcTransportUnitTest, RejectsEnvelopeWithBothResultAndError) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{
            .status_code = kHttpOk,
            .headers = {{"A2A-Version", "1.0"}},
            .body =
                R"({"jsonrpc":"2.0","id":"req-123","result":{"id":"t-1"},"error":{"code":-32000,"message":"bad"}})"};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");
  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kRemoteProtocol);
}

TEST(JsonRpcTransportUnitTest, ValidatesRequiredIdsForTaskAndPushConfigOperations) {
  A2AClient client(std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Internal("requester should not be called");
      }));

  const auto expect_validation = [](const auto& result) {
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code(), ErrorCode::kValidation);
  };

  expect_validation(client.GetTask(lf::a2a::v1::GetTaskRequest{}));
  expect_validation(client.CancelTask(lf::a2a::v1::CancelTaskRequest{}));
  expect_validation(client.GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest{}));
  expect_validation(client.DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest{}));
}

TEST(JsonRpcTransportUnitTest, ReturnsInternalErrorWhenStreamingRequesterMissing) {
  A2AClient client(std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Internal("requester should not be called");
      }));

  class TestObserver final : public a2a::client::StreamObserver {
   public:
    void OnEvent(const lf::a2a::v1::StreamResponse& response) override { (void)response; }
    void OnError(const a2a::core::Error& error) override { (void)error; }
    void OnCompleted() override {}
  } observer;

  lf::a2a::v1::SendMessageRequest send_request;
  const auto stream_response = client.SendStreamingMessage(send_request, observer);
  ASSERT_FALSE(stream_response.ok());
  EXPECT_EQ(stream_response.error().code(), ErrorCode::kInternal);

  lf::a2a::v1::GetTaskRequest subscribe_request;
  const auto subscribe_response = client.SubscribeTask(subscribe_request, observer);
  ASSERT_FALSE(subscribe_response.ok());
  EXPECT_EQ(subscribe_response.error().code(), ErrorCode::kValidation);
}

TEST(JsonRpcTransportUnitTest, PropagatesConfiguredRequestHeadersAndExtensions) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = SuccessGetTaskEnvelope("req-headers")};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-headers"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  CallOptions options;
  options.headers["X-Trace-Id"] = "trace-1";
  options.extensions = {"ext.alpha", "ext.beta"};

  const auto response = client.GetTask(request, options);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(captured.headers.at("X-Trace-Id"), "trace-1");
  EXPECT_TRUE(captured.headers.contains("A2A-Extensions"));
}

}  // namespace

namespace {

constexpr std::chrono::milliseconds kStreamWaitTimeout{1000};

class JsonRpcRecordingObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    std::lock_guard<std::mutex> lock(mu_);
    events.push_back(response);
  }

  void OnError(const a2a::core::Error& error) override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      errors.push_back(error);
    }
    cv_.notify_all();
  }

  void OnCompleted() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      completed = true;
    }
    cv_.notify_all();
  }

  bool Wait() {
    std::unique_lock<std::mutex> lock(mu_);
    return cv_.wait_for(lock, kStreamWaitTimeout, [this] { return completed || !errors.empty(); });
  }

  std::mutex mu_;
  std::condition_variable cv_;
  std::vector<lf::a2a::v1::StreamResponse> events;
  std::vector<a2a::core::Error> errors;
  bool completed = false;
};

a2a::core::Result<HttpClientResponse> EmitJsonRpcMetadataOnly(
    HttpClientResponse response, const a2a::client::HttpStreamMetadataHandler& on_metadata) {
  const auto metadata = on_metadata(response);
  if (!metadata.ok()) {
    return metadata.error();
  }
  return response;
}

void ExpectSuccessfulStream(const HttpRequest& captured, JsonRpcRecordingObserver& observer) {
  EXPECT_EQ(captured.headers.at("Accept"), "text/event-stream");
  EXPECT_EQ(captured.headers.at("Content-Type"), "application/json");
  ASSERT_TRUE(observer.errors.empty()) << observer.errors.front().message();
  EXPECT_TRUE(observer.completed);
  ASSERT_EQ(observer.events.size(), 1U);
  EXPECT_EQ(observer.events.front().task().id(), "task-1");
}

TEST(JsonRpcTransportUnitTest, SendStreamingMessageParsesJsonRpcSseEnvelope) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [&captured](const HttpRequest& request, const a2a::client::HttpStreamMetadataHandler& on_metadata,
                  const a2a::client::HttpStreamChunkHandler& on_chunk,
                  const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        HttpClientResponse response{.status_code = kHttpOk,
                                    .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                    .body = ""};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return metadata.error();
        }
        const auto chunk = on_chunk(
            R"(data: {"jsonrpc":"2.0","id":"stream-1","result":{"task":{"id":"task-1"}}}

)");
        if (!chunk.ok()) {
          return chunk.error();
        }
        return response;
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  ExpectSuccessfulStream(captured, observer);
}

TEST(JsonRpcTransportUnitTest, StreamingRejectsContentTypePrefixLookalike) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
         const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return EmitJsonRpcMetadataOnly(
            HttpClientResponse{.status_code = kHttpOk,
                               .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-streaming"}},
                               .body = ""},
            on_metadata);
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_FALSE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), ErrorCode::kRemoteProtocol);
}

TEST(JsonRpcTransportUnitTest, EmptyResponseWithoutMetadataCallbackStillCompletes) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_TRUE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  EXPECT_TRUE(observer.errors.empty());
}

TEST(JsonRpcTransportUnitTest, EmptyStreamValidatesReturnedUnsupportedVersion) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "2.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_FALSE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), ErrorCode::kUnsupportedVersion);
}

TEST(JsonRpcTransportUnitTest, EmptyStreamValidatesReturnedNonSuccessStatus) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpBadGateway,
                                  .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                  .body = ""};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_FALSE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), ErrorCode::kRemoteProtocol);
  EXPECT_EQ(observer.errors.front().http_status().value_or(0), kHttpBadGateway);
}

TEST(JsonRpcTransportUnitTest, EmptyStreamValidatesReturnedMissingContentType) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler&, const a2a::client::HttpStreamChunkHandler&,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = ""};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "stream-1"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_FALSE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  ASSERT_EQ(observer.errors.size(), 1U);
  EXPECT_EQ(observer.errors.front().code(), ErrorCode::kRemoteProtocol);
}

TEST(JsonRpcTransportUnitTest, StreamingMismatchedResponseIdReportsErrorOnce) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> { return a2a::core::Error::Internal("unused"); },
      [](const HttpRequest&, const a2a::client::HttpStreamMetadataHandler& on_metadata,
         const a2a::client::HttpStreamChunkHandler& on_chunk,
         const a2a::client::StreamCancelled&) -> a2a::core::Result<HttpClientResponse> {
        HttpClientResponse response{.status_code = kHttpOk,
                                    .headers = {{"A2A-Version", "1.0"}, {"Content-Type", "text/event-stream"}},
                                    .body = ""};
        const auto metadata = on_metadata(response);
        if (!metadata.ok()) {
          return metadata.error();
        }
        const auto chunk = on_chunk(
            R"(data: {"jsonrpc":"2.0","id":"wrong-id","result":{"task":{"id":"task-1"}}}

)");
        if (!chunk.ok()) {
          return chunk.error();
        }
        return response;
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "expected-id"; });

  A2AClient client(std::move(transport));
  lf::a2a::v1::SendMessageRequest request;
  JsonRpcRecordingObserver observer;

  auto stream = client.SendStreamingMessage(request, observer);
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.Wait());

  EXPECT_FALSE(observer.completed);
  EXPECT_TRUE(observer.events.empty());
  EXPECT_EQ(observer.errors.size(), 1U);
}

}  // namespace
