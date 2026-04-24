#include <gtest/gtest.h>

#include <string>

#include "a2a/core/protojson.h"
#include "a2a/server/json_rpc_server_transport.h"
#include "a2a/server/server.h"
#include "../support/rest_server_test_utils.h"

namespace {

TEST(JsonRpcServerTransportFunctionalTest, SupportsTaskLifecycleMethodsOverJsonRpc) {
  a2a::server::InMemoryTaskStore store;
  a2a::tests::support::StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto send_response = server.Handle(
      a2a::tests::support::MakeHttpRequest(
          "POST", "/rpc", {{"A2A-Version", "1.0"}},
          R"({"jsonrpc":"2.0","id":"send-1","method":"a2a.sendMessage","params":{"message":{"role":"user","taskId":"task-jsonrpc-functional-1"}}})"));
  ASSERT_TRUE(send_response.ok());
  EXPECT_EQ(send_response.value().status_code, 200);

  const auto get_response = server.Handle(
      a2a::tests::support::MakeHttpRequest(
          "POST", "/rpc", {{"A2A-Version", "1.0"}},
          R"({"jsonrpc":"2.0","id":"get-1","method":"a2a.getTask","params":{"id":"task-jsonrpc-functional-1"}})"));
  ASSERT_TRUE(get_response.ok());
  EXPECT_EQ(get_response.value().status_code, 200);
  EXPECT_NE(get_response.value().body.find("task-jsonrpc-functional-1"), std::string::npos);

  const auto cancel_response = server.Handle(
      a2a::tests::support::MakeHttpRequest(
          "POST", "/rpc", {{"A2A-Version", "1.0"}},
          R"({"jsonrpc":"2.0","id":"cancel-1","method":"a2a.cancelTask","params":{"id":"task-jsonrpc-functional-1"}})"));
  ASSERT_TRUE(cancel_response.ok());
  EXPECT_EQ(cancel_response.value().status_code, 200);

  const auto list_response = server.Handle(
      a2a::tests::support::MakeHttpRequest(
          "POST", "/rpc", {{"A2A-Version", "1.0"}},
          R"({"jsonrpc":"2.0","id":101,"method":"a2a.listTasks","params":{"pageSize":10}})"));
  ASSERT_TRUE(list_response.ok());
  EXPECT_EQ(list_response.value().status_code, 200);
  EXPECT_NE(list_response.value().body.find("task-jsonrpc-functional-1"), std::string::npos);
  EXPECT_NE(list_response.value().body.find("\"id\":101"), std::string::npos);
}

TEST(JsonRpcServerTransportFunctionalTest, RejectsMissingVersionHeaderWhenRequired) {
  a2a::server::InMemoryTaskStore store;
  a2a::tests::support::StoreExecutor executor(&store);
  a2a::server::Dispatcher dispatcher(&executor);
  a2a::server::JsonRpcServerTransport server(&dispatcher, {.rpc_path = "/rpc"});

  const auto response = server.Handle(
      a2a::tests::support::MakeHttpRequest(
          "POST", "/rpc", {},
          R"({"jsonrpc":"2.0","id":"send-no-version","method":"a2a.sendMessage","params":{"message":{"role":"user","taskId":"task-a"}}})"));

  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().status_code, 426);
  EXPECT_NE(response.value().body.find("Missing required A2A-Version header"), std::string::npos);
}

}  // namespace
