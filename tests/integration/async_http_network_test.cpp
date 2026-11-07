// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#if defined(A2A_HAS_LIBCURL)
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "a2a/client/discovery.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "a2a/core/http_constants.h"
#include "support/curl_reactor_test_support.h"

namespace {
using namespace a2a;
using http::testing::CurlLoopbackServer;
constexpr auto kWait = std::chrono::seconds(5);
constexpr auto kShort = std::chrono::milliseconds(20);
constexpr auto kLong = std::chrono::seconds(30);
constexpr auto kTaskId = "async-task";
constexpr auto kRequestId = "async-request";
constexpr auto kTaskBody = R"({"id":"async-task"})";
constexpr auto kRpcBody = R"({"jsonrpc":"2.0","id":"async-request","result":{"id":"async-task"}})";
constexpr auto kNoChunk = "";
constexpr auto kCancelled = "CANCELLED";
constexpr auto kDeadline = "DEADLINE_EXCEEDED";
constexpr std::size_t kOverLimit = (std::size_t{16U} * 1024U * 1024U) + 1U;
constexpr std::size_t kMixedRequests = 2;
constexpr std::size_t kStreamIndex = 0;
constexpr std::size_t kUnaryIndex = 1;
constexpr std::size_t kCalls = 64;
constexpr std::size_t kThreadBound = 9;
constexpr auto kUnknownVersion = "HTTP/0.9";

http::Request RequestFor(const std::string& url) {
  http::Request request;
  request.method = std::string(core::http::kMethodGet);
  request.url = url;
  request.timeout = kLong;
  return request;
}

void ObserveResponse(const core::Result<http::Response>& response, std::atomic<std::size_t>& completed) {
  EXPECT_TRUE(response.ok());
  if (response.ok()) {
    EXPECT_EQ(response.value().body, kTaskBody);
  }
  ++completed;
}
struct ConcurrentRequests final {
  ~ConcurrentRequests() { client.Shutdown(); }
  std::atomic<std::size_t> completed{0};
  http::Client client;
  std::vector<core::AsyncHandle> handles;
  void Start(const std::string& url) {
    for (std::size_t index = 0; index < kCalls; ++index) {
      auto operation =
          client.SendRequestAsync(RequestFor(url), [this](auto response) { ObserveResponse(response, completed); });
      if (!operation.ok()) {
        ADD_FAILURE() << operation.error().message();
        continue;
      }
      handles.push_back(std::move(operation.value()));
    }
  }
  void Wait() const {
    for (const auto& handle : handles) {
      handle.Wait();
    }
  }
};
void FinishResponses(CurlLoopbackServer& server) {
  for (std::size_t index = 0; index < kCalls; ++index) {
    EXPECT_TRUE(server.Finish(index, kNoChunk));
  }
}
TEST(AsyncHttpNetworkTest, SimultaneousCallsUseBoundedThreadsAndSharedReactors) {
  CurlLoopbackServer server(kCalls, kTaskBody);
  ConcurrentRequests requests;
  const auto threads = http::testing::ReactorTestThreadCount();
  requests.Start(server.url());
  ASSERT_TRUE(server.WaitForStreams(kCalls));
  EXPECT_LE(http::testing::ReactorTestThreadCount(), threads + kThreadBound);
  FinishResponses(server);
  requests.Wait();
  EXPECT_EQ(requests.completed.load(), kCalls);
}

void FinishUnary(CurlLoopbackServer& server, std::future<core::Result<http::Response>>& response) {
  ASSERT_TRUE(server.WaitForStreams(kMixedRequests));
  ASSERT_TRUE(server.Finish(kUnaryIndex, kNoChunk));
  ASSERT_EQ(response.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(response.get().ok());
}

template <typename Stream>
void FinishOpenStream(CurlLoopbackServer& server, Stream& stream) {
  EXPECT_EQ(stream.result.wait_for(std::chrono::seconds::zero()), std::future_status::timeout);
  ASSERT_TRUE(server.Finish(kStreamIndex, kNoChunk));
  ASSERT_EQ(stream.result.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(stream.result.get().ok());
}

TEST(AsyncHttpNetworkTest, UnaryCompletesWhileSameClientStreamRemainsOpen) {
  CurlLoopbackServer server(kMixedRequests, kTaskBody);
  http::Client client;
  auto stream = http::testing::StartTestStream(client, server.url());
  ASSERT_TRUE(server.WaitForStreams(1));
  ASSERT_TRUE(stream->WaitForChunk());
  auto result = std::make_shared<std::promise<core::Result<http::Response>>>();
  auto future = result->get_future();
  auto operation = client.SendRequestAsync(RequestFor(server.url()),
                                           [result](auto response) { result->set_value(std::move(response)); });
  ASSERT_TRUE(operation.ok());
  FinishUnary(server, future);
  FinishOpenStream(server, *stream);
  operation.value().Wait();
}

TEST(AsyncHttpNetworkTest, CancelStopsOpenResponseAndCompletesOnce) {
  CurlLoopbackServer server(1, kTaskBody);
  http::Client client;
  std::promise<core::Result<http::Response>> result;
  auto future = result.get_future();
  auto operation =
      client.SendRequestAsync(RequestFor(server.url()), [&](auto response) { result.set_value(std::move(response)); });
  ASSERT_TRUE(operation.ok());
  ASSERT_TRUE(server.WaitForStreams(1));
  operation.value().Cancel();
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kCancelled);
  operation.value().Wait();
}

TEST(AsyncHttpNetworkTest, DeadlineStopsOpenResponse) {
  CurlLoopbackServer server(1, kTaskBody);
  http::Client client;
  std::promise<core::Result<http::Response>> result;
  auto future = result.get_future();
  auto request = RequestFor(server.url());
  request.timeout = kShort;
  auto operation =
      client.SendRequestAsync(std::move(request), [&](auto response) { result.set_value(std::move(response)); });
  ASSERT_TRUE(operation.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kDeadline);
  operation.value().Wait();
}

TEST(AsyncHttpNetworkTest, ShutdownDrainsAndRejectsNewRequests) {
  CurlLoopbackServer server(1, kTaskBody);
  http::Client client;
  std::atomic<std::size_t> completed{0};
  auto operation = client.SendRequestAsync(RequestFor(server.url()), [&](auto response) {
    EXPECT_FALSE(response.ok());
    ++completed;
  });
  ASSERT_TRUE(operation.ok());
  ASSERT_TRUE(server.WaitForStreams(1));
  client.Shutdown();
  EXPECT_EQ(completed.load(), 1U);
  EXPECT_FALSE(client.SendRequestAsync(RequestFor(server.url()), [](auto response) { (void)response; }).ok());
}

TEST(AsyncHttpNetworkTest, InvalidHttpVersionCompletesWithValidationError) {
  http::Client client;
  auto request = RequestFor(std::string(http::testing::kLoopbackUrlPrefix));
  request.http_version = kUnknownVersion;
  std::promise<core::Result<http::Response>> result;
  auto future = result.get_future();
  auto operation =
      client.SendRequestAsync(std::move(request), [&](auto response) { result.set_value(std::move(response)); });
  ASSERT_TRUE(operation.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), core::ErrorCode::kValidation);
  operation.value().Wait();
}

TEST(AsyncHttpNetworkTest, ResponseBodyLimitCompletesWithError) {
  CurlLoopbackServer server(1, std::string(kOverLimit, 'x'));
  http::Client client;
  std::promise<core::Result<http::Response>> result;
  auto future = result.get_future();
  auto operation =
      client.SendRequestAsync(RequestFor(server.url()), [&](auto response) { result.set_value(std::move(response)); });
  ASSERT_TRUE(operation.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), core::ErrorCode::kNetwork);
  operation.value().Wait();
}

void ExpectTask(const core::Result<lf::a2a::v1::Task>& response) {
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response.value().id(), kTaskId);
}

class DefaultAsyncHttpTransportTest : public testing::TestWithParam<client::PreferredTransport> {};
TEST_P(DefaultAsyncHttpTransportTest, DefaultFactoryUsesSharedHttpAsyncEngine) {
  const bool rpc = GetParam() == client::PreferredTransport::kJsonRpc;
  CurlLoopbackServer server(1, rpc ? kRpcBody : kTaskBody);
  client::ResolvedInterface resolved{
      .transport = GetParam(), .url = server.url(), .security_requirements = {}, .security_schemes = {}};
  std::unique_ptr<client::ClientTransport> transport;
  if (rpc) {
    transport = client::JsonRpcTransport::CreateDefault(resolved, kWait, [] { return std::string(kRequestId); });
  } else {
    transport = client::HttpJsonTransport::CreateDefault(resolved, kWait);
  }
  client::A2AClient client(std::move(transport));
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  auto operation = client.GetTaskAsync(request, [&](auto response) { result.set_value(std::move(response)); });
  ASSERT_TRUE(operation.ok());
  ASSERT_TRUE(server.WaitForStreams(1));
  ASSERT_TRUE(server.Finish(0, kNoChunk));
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ExpectTask(response);
  operation.value().Wait();
}
INSTANTIATE_TEST_SUITE_P(HttpBindings, DefaultAsyncHttpTransportTest,
                         testing::Values(client::PreferredTransport::kRest, client::PreferredTransport::kJsonRpc));
}  // namespace
#endif
