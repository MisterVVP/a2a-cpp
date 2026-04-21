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

ResolvedInterface MakeResolvedJsonRpc() {
  ResolvedInterface resolved;
  resolved.transport = PreferredTransport::kJsonRpc;
  resolved.url = "https://agent.example.com/rpc";
  return resolved;
}

TEST(JsonRpcClientFunctionalTest, SupportsCoreUnaryOperationsWithStableContract) {
  auto transport = std::make_unique<JsonRpcTransport>(
      MakeResolvedJsonRpc(),
      [](const HttpRequest& request) -> a2a::core::Result<HttpClientResponse> {
        google::protobuf::Struct envelope;
        const auto parsed = a2a::core::JsonToMessage(request.body, &envelope);
        if (!parsed.ok()) {
          return parsed.error();
        }
        const std::string id = envelope.fields().at("id").string_value();
        const std::string method = envelope.fields().at("method").string_value();

        if (method == "a2a.sendMessage") {
          return HttpClientResponse{.status_code = 200,
                                    .headers = {{"A2A-Version", "1.0"}},
                                    .body = "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
                                            "\",\"result\":{\"message\":{\"role\":\"agent\"}}}"};
        }
        if (method == "a2a.getTask") {
          return HttpClientResponse{
              .status_code = 200,
              .headers = {{"A2A-Version", "1.0"}},
              .body = "{\"jsonrpc\":\"2.0\",\"id\":\"" + id + "\",\"result\":{\"id\":\"t-1\"}}"};
        }
        if (method == "a2a.cancelTask") {
          return HttpClientResponse{
              .status_code = 200,
              .headers = {{"A2A-Version", "1.0"}},
              .body =
                  "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
                  "\",\"result\":{\"id\":\"t-1\",\"status\":{\"state\":\"TASK_STATE_CANCELED\"}}}"};
        }
        return HttpClientResponse{
            .status_code = 200,
            .headers = {{"A2A-Version", "1.0"}},
            .body = "{\"jsonrpc\":\"2.0\",\"id\":\"" + id +
                    "\",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}"};
      });

  A2AClient client(std::move(transport));

  lf::a2a::v1::SendMessageRequest send_request;
  send_request.mutable_message()->set_role("user");
  const auto send_response = client.SendMessage(send_request);
  ASSERT_TRUE(send_response.ok()) << send_response.error().message();
  ASSERT_TRUE(send_response.value().has_message());
  EXPECT_EQ(send_response.value().message().role(), "agent");

  lf::a2a::v1::GetTaskRequest get_request;
  get_request.set_id("t-1");
  const auto get_response = client.GetTask(get_request);
  ASSERT_TRUE(get_response.ok()) << get_response.error().message();
  EXPECT_EQ(get_response.value().id(), "t-1");

  lf::a2a::v1::CancelTaskRequest cancel_request;
  cancel_request.set_id("t-1");
  const auto cancel_response = client.CancelTask(cancel_request);
  ASSERT_TRUE(cancel_response.ok()) << cancel_response.error().message();
  EXPECT_EQ(cancel_response.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

}  // namespace
