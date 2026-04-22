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

constexpr int kHttpOk = 200;
constexpr int kMethodNotFound = -32601;

ResolvedInterface MakeResolvedJsonRpc() {
  ResolvedInterface resolved;
  resolved.transport = PreferredTransport::kJsonRpc;
  resolved.url = "https://agent.example.com/rpc";
  return resolved;
}

std::string BuildResultEnvelope(std::string_view id, std::string_view result_json) {
  return std::string(R"({"jsonrpc":"2.0","id":")") + std::string(id) +
         std::string(R"(","result":)") + std::string(result_json) + "}";
}

std::string BuildErrorEnvelope(std::string_view id, int code, std::string_view message) {
  return std::string(R"({"jsonrpc":"2.0","id":")") + std::string(id) +
         std::string(R"(","error":{"code":)") + std::to_string(code) +
         std::string(R"(,"message":")") + std::string(message) + R"("}})";
}

std::string ExtractRequestId(const std::string& json_payload) {
  google::protobuf::Struct envelope;
  const auto parsed = a2a::core::JsonToMessage(json_payload, &envelope);
  if (!parsed.ok()) {
    return {};
  }
  const auto id = envelope.fields().find("id");
  if (id == envelope.fields().end() || !id->second.has_string_value()) {
    return {};
  }
  return id->second.string_value();
}

TEST(JsonRpcClientIntegrationTest, MapsRemoteJsonRpcErrorObject) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = BuildErrorEnvelope(ExtractRequestId(request.body),
                                                             kMethodNotFound, "Method not found")};
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
        return HttpClientResponse{
            .status_code = kHttpOk,
            .headers = {{"A2A-Version", "1.0"}},
            .body = BuildResultEnvelope(ExtractRequestId(request.body), R"({"id":123})")};
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
        return HttpClientResponse{.status_code = kHttpOk,
                                  .headers = {{"A2A-Version", "1.0"}},
                                  .body = BuildErrorEnvelope(ExtractRequestId(request.body),
                                                             kMethodNotFound, "Method not found")};
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
