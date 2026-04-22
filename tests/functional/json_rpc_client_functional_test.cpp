#include <google/protobuf/struct.pb.h>
#include <gtest/gtest.h>

#include <string>

#include "a2a/client/client.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/protojson.h"

namespace {

using a2a::client::A2AClient;
using a2a::client::HttpClientResponse;
using a2a::client::HttpRequest;
using a2a::client::JsonRpcTransport;
using a2a::client::PreferredTransport;
using a2a::client::ResolvedInterface;

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

std::string ExtractFieldOrDefault(const google::protobuf::Struct& object, std::string_view field) {
  const auto value = object.fields().find(std::string(field));
  if (value == object.fields().end() || !value->second.has_string_value()) {
    return {};
  }
  return value->second.string_value();
}

a2a::core::Result<HttpClientResponse> HandleFunctionalRequest(const HttpRequest& request) {
  google::protobuf::Struct envelope;
  const auto parsed = a2a::core::JsonToMessage(request.body, &envelope);
  if (!parsed.ok()) {
    return parsed.error();
  }

  const std::string id = ExtractFieldOrDefault(envelope, "id");
  const std::string method = ExtractFieldOrDefault(envelope, "method");
  if (method == "a2a.sendMessage") {
    return HttpClientResponse{.status_code = kHttpOk,
                              .headers = {{"A2A-Version", "1.0"}},
                              .body = BuildResultEnvelope(id, R"({"message":{"role":"agent"}})")};
  }
  if (method == "a2a.getTask") {
    return HttpClientResponse{.status_code = kHttpOk,
                              .headers = {{"A2A-Version", "1.0"}},
                              .body = BuildResultEnvelope(id, R"({"id":"t-1"})")};
  }
  if (method == "a2a.cancelTask") {
    return HttpClientResponse{.status_code = kHttpOk,
                              .headers = {{"A2A-Version", "1.0"}},
                              .body = BuildResultEnvelope(
                                  id, R"({"id":"t-1","status":{"state":"TASK_STATE_CANCELED"}})")};
  }

  return HttpClientResponse{.status_code = kHttpOk,
                            .headers = {{"A2A-Version", "1.0"}},
                            .body = BuildErrorEnvelope(id, kMethodNotFound, "Method not found")};
}

TEST(JsonRpcClientFunctionalTest, SendMessageRoundTripsThroughTransportContract) {
  auto transport =
      std::make_unique<JsonRpcTransport>(MakeResolvedJsonRpc(), HandleFunctionalRequest);
  A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_role("user");

  const auto response = client.SendMessage(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  ASSERT_TRUE(response.value().has_message());
  EXPECT_EQ(response.value().message().role(), "agent");
}

TEST(JsonRpcClientFunctionalTest, GetTaskRoundTripsThroughTransportContract) {
  auto transport =
      std::make_unique<JsonRpcTransport>(MakeResolvedJsonRpc(), HandleFunctionalRequest);
  A2AClient client(std::move(transport));

  lf::a2a::v1::GetTaskRequest request;
  request.set_id("t-1");

  const auto response = client.GetTask(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), "t-1");
}

TEST(JsonRpcClientFunctionalTest, CancelTaskRoundTripsThroughTransportContract) {
  auto transport =
      std::make_unique<JsonRpcTransport>(MakeResolvedJsonRpc(), HandleFunctionalRequest);
  A2AClient client(std::move(transport));

  lf::a2a::v1::CancelTaskRequest request;
  request.set_id("t-1");

  const auto response = client.CancelTask(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

}  // namespace
