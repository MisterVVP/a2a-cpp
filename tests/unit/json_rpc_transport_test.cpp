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

ResolvedInterface MakeResolvedJsonRpc() {
  ResolvedInterface resolved;
  resolved.transport = PreferredTransport::kJsonRpc;
  resolved.url = "https://agent.example.com/rpc";
  return resolved;
}

TEST(JsonRpcTransportUnitTest, SerializesEnvelopeWithStrictRequestIdAndMethodName) {
  HttpRequest captured;
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [&captured](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        captured = request;
        return HttpClientResponse{
            .status_code = 200,
            .headers = {{"A2A-Version", "1.0"}},
            .body = R"({"jsonrpc":"2.0","id":"req-123","result":{"id":"t-1"}})"};
      },
      JsonRpcTransport::kDefaultTimeout, [] { return "req-123"; });

  A2AClient client(std::move(transport));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  CallOptions options;
  options.timeout = std::chrono::milliseconds(1200);

  const auto response = client.GetTask(request, options);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), "t-1");

  EXPECT_EQ(captured.method, "POST");
  EXPECT_EQ(captured.url, "https://agent.example.com/rpc");
  EXPECT_EQ(captured.timeout, std::chrono::milliseconds(1200));

  google::protobuf::Struct envelope;
  const auto parsed = a2a::core::JsonToMessage(captured.body, &envelope);
  ASSERT_TRUE(parsed.ok()) << parsed.error().message();
  EXPECT_EQ(envelope.fields().at("jsonrpc").string_value(), "2.0");
  EXPECT_EQ(envelope.fields().at("id").string_value(), "req-123");
  EXPECT_EQ(envelope.fields().at("method").string_value(), "a2a.getTask");
  EXPECT_TRUE(envelope.fields().contains("params"));
}

TEST(JsonRpcTransportUnitTest, ResponseIdMismatchReturnsRemoteProtocolError) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{
            .status_code = 200,
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
            .status_code = 200, .headers = {{"A2A-Version", "1.0"}}, .body = "{not-json"};
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
