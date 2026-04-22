#include "a2a/client/json_rpc_transport.h"

#include <google/protobuf/struct.pb.h>
#include <gtest/gtest.h>

#include <chrono>
#include <string>

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
constexpr std::chrono::milliseconds kCustomTimeout{1200};

ResolvedInterface MakeResolvedJsonRpc() {
  ResolvedInterface resolved;
  resolved.transport = PreferredTransport::kJsonRpc;
  resolved.url = "https://agent.example.com/rpc";
  return resolved;
}

std::string SuccessGetTaskEnvelope(std::string_view request_id) {
  return std::string(R"({"jsonrpc":"2.0","id":")") + std::string(request_id) +
         R"(","result":{"id":"t-1"}})";
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
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = SuccessGetTaskEnvelope("req-123")};
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
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = SuccessGetTaskEnvelope("req-123")};
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
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = SuccessGetTaskEnvelope("req-123")};
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
        return HttpClientResponse{
            .status_code = kHttpOk,
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
        return HttpClientResponse{
            .status_code = kHttpOk, .headers = {{"A2A-Version", "1.0"}}, .body = "{not-json"};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kSerialization);
  EXPECT_EQ(response.error().transport().value_or(""), "jsonrpc");
}

}  // namespace
