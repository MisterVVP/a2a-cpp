#include <google/protobuf/struct.pb.h>
#include <gtest/gtest.h>

#include <string>

#include "a2a/client/client.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/error.h"
#include "a2a/core/protojson.h"

namespace {

using a2a::client::A2AClient;
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

std::string ExtractRequestId(const std::string& json_payload) {
  google::protobuf::Struct envelope;
  const auto parsed = a2a::core::JsonToMessage(json_payload, &envelope);
  if (!parsed.ok()) {
    return {};
  }
  return envelope.fields().at("id").string_value();
}

TEST(JsonRpcClientIntegrationTest, MapsRemoteJsonRpcErrorObject) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        const std::string id = ExtractRequestId(request.body);
        return HttpClientResponse{
            .status_code = 200,
            .headers = {{"A2A-Version", "1.0"}},
            .body = "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
                    "\",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}"};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kRemoteProtocol);
  EXPECT_EQ(response.error().protocol_code().value_or(""), "-32601");
}

TEST(JsonRpcClientIntegrationTest, InvalidResultPayloadReturnsSerializationError) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        const std::string id = ExtractRequestId(request.body);
        return HttpClientResponse{
            .status_code = 200,
            .headers = {{"A2A-Version", "1.0"}},
            .body = "{\"jsonrpc\":\"2.0\",\"id\":\"" + id + "\",\"result\":{\"id\":123}}"};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kSerialization);
  EXPECT_EQ(response.error().transport().value_or(""), "jsonrpc");
}

TEST(JsonRpcClientIntegrationTest, TimeoutOrNetworkFailureBubblesUp) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(), [](const HttpRequest&) -> a2a::core::Result<HttpClientResponse> {
        return a2a::core::Error::Network("request timed out");
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kNetwork);
}

TEST(JsonRpcClientIntegrationTest, UnsupportedMethodErrorIsSurfacedForDeleteConfig) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        const std::string id = ExtractRequestId(request.body);
        return HttpClientResponse{
            .status_code = 200,
            .headers = {{"A2A-Version", "1.0"}},
            .body = "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
                    "\",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}"};
      });

  A2AClient client(std::move(transport));
  lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request;
  request.set_id("pn-1");

  const auto response = client.DeleteTaskPushNotificationConfig(request);
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), ErrorCode::kRemoteProtocol);
  EXPECT_EQ(response.error().protocol_code().value_or(""), "-32601");
}

}  // namespace
