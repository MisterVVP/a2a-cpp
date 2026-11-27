// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/client/json_rpc_transport.h"
#include "support/curl_reactor_test_support.h"

namespace {

using namespace a2a::http::testing;
using a2a::client::PreferredTransport;
constexpr std::string_view kStreamTaskId = "reactor-task";
constexpr std::string_view kStreamRequestId = "reactor-request";
constexpr std::string_view kRestFirst =
    R"(data: {"statusUpdate":{"taskId":"reactor-task","status":{"state":"TASK_STATE_WORKING"}}})"
    "\n\n";
constexpr std::string_view kRestLast =
    R"(data: {"statusUpdate":{"taskId":"reactor-task","status":{"state":"TASK_STATE_COMPLETED"}}})"
    "\n\n";
constexpr std::string_view kRpcFirst =
    R"(data: {"jsonrpc":"2.0","id":"reactor-request","result":{"statusUpdate":{"taskId":"reactor-task","status":{"state":"TASK_STATE_WORKING"}}}})"
    "\n\n";
constexpr std::string_view kRpcLast =
    R"(data: {"jsonrpc":"2.0","id":"reactor-request","result":{"statusUpdate":{"taskId":"reactor-task","status":{"state":"TASK_STATE_COMPLETED"}}}})"
    "\n\n";

class ReactorObserver final : public a2a::client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& response) override {
    EXPECT_TRUE(a2a::http::detail::IsDispatchingStreamCallback(nullptr));
    {
      std::lock_guard lock(mutex_);
      states_.push_back(response.status_update().status().state());
    }
    changed_.notify_all();
  }
  void OnError(const a2a::core::Error& error) override {
    (void)error;
    {
      std::lock_guard lock(mutex_);
      ++errors_;
    }
    changed_.notify_all();
  }
  void OnCompleted() override {
    EXPECT_TRUE(a2a::http::detail::IsDispatchingStreamCallback(nullptr));
    {
      std::lock_guard lock(mutex_);
      ++completions_;
    }
    changed_.notify_all();
  }
  bool WaitForFirst() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kReactorTestTimeout, [this] { return !states_.empty(); });
  }
  bool WaitForCompletion() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kReactorTestTimeout, [this] { return completions_ != 0 || errors_ != 0; });
  }
  void ExpectOrderedCompletion() {
    std::lock_guard lock(mutex_);
    EXPECT_EQ(states_, (std::vector{lf::a2a::v1::TASK_STATE_WORKING, lf::a2a::v1::TASK_STATE_COMPLETED}));
    EXPECT_EQ(errors_, 0U);
    EXPECT_EQ(completions_, 1U);
  }
  void ExpectCancelled() {
    std::lock_guard lock(mutex_);
    EXPECT_EQ(states_, (std::vector{lf::a2a::v1::TASK_STATE_WORKING}));
    EXPECT_EQ(errors_, 0U);
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<lf::a2a::v1::TaskState> states_;
  std::size_t errors_ = 0;
  std::size_t completions_ = 0;
};

std::unique_ptr<a2a::client::ClientTransport> MakeReactorTransport(PreferredTransport kind, std::string url) {
  a2a::client::ResolvedInterface resolved;
  resolved.transport = kind;
  resolved.url = std::move(url);
  if (kind == PreferredTransport::kRest) {
    return a2a::client::HttpJsonTransport::CreateDefault(resolved);
  }
  return a2a::client::JsonRpcTransport::CreateDefault(resolved, a2a::client::JsonRpcTransport::kDefaultTimeout,
                                                      [] { return std::string(kStreamRequestId); });
}

class CurlStreamingTransportTest : public ::testing::TestWithParam<PreferredTransport> {};

TEST_P(CurlStreamingTransportTest, RealSsePreservesOrderAndDispatchesOffReactor) {
  const bool rest = GetParam() == PreferredTransport::kRest;
  CurlLoopbackServer server(1U, rest ? kRestFirst : kRpcFirst);
  auto transport = MakeReactorTransport(GetParam(), server.url());
  ReactorObserver observer;
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kStreamTaskId));
  auto stream = transport->SubscribeTask(request, observer, {});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForFirst());
  ASSERT_TRUE(server.Finish(0, rest ? kRestLast : kRpcLast));
  ASSERT_TRUE(observer.WaitForCompletion());
  observer.ExpectOrderedCompletion();
}

TEST_P(CurlStreamingTransportTest, IdleCancelIsPromptAndProducesNoObserverError) {
  const bool rest = GetParam() == PreferredTransport::kRest;
  CurlLoopbackServer server(1U, rest ? kRestFirst : kRpcFirst);
  auto transport = MakeReactorTransport(GetParam(), server.url());
  ReactorObserver observer;
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(std::string(kStreamTaskId));
  auto stream = transport->SubscribeTask(request, observer, {});
  ASSERT_TRUE(stream.ok()) << stream.error().message();
  ASSERT_TRUE(observer.WaitForFirst());
  stream.value()->Cancel();
  EXPECT_FALSE(stream.value()->IsActive());
  observer.ExpectCancelled();
}

INSTANTIATE_TEST_SUITE_P(Protocols, CurlStreamingTransportTest,
                         ::testing::Values(PreferredTransport::kRest, PreferredTransport::kJsonRpc));

}  // namespace
