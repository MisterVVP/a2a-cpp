// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/core/http_constants.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/version.h"
#include "a2a/server/engine/async_runtime.h"
#include "sut/sut_agent.h"
#include "sut/sut_coroutine_executor.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
using namespace a2a::tests::sut;
constexpr auto kWait = std::chrono::seconds(5);
constexpr auto kEndpoint = "127.0.0.1:1";
constexpr auto kTaskId = "async-sut-task";
constexpr auto kToken = "async-sut-token";
constexpr auto kBearerToken = "bearer_token";
constexpr auto kTimeout = "async SUT test timed out";
constexpr std::size_t kCalls = 8;
constexpr std::size_t kSingleBinding = 1;

SutAgentOptions GrpcOptions() {
  SutAgentOptions options;
  options.asynchronous = true;
  options.streaming = true;
  return options;
}
server::stores::StoreBundle Stores() {
  auto stores = server::stores::InMemoryStoreFactory{}.CreateStoreBundle();
  EXPECT_TRUE(stores.ok());
  return std::move(stores.value());
}
class AsyncFixture final {
 public:
  AsyncFixture(stores::StoreBundle stores, const SutEndpoints& endpoints, SutAgentOptions options)
      : agent_(std::move(stores), endpoints, std::move(options)),
        runtime_(std::make_shared<SutCoroutineExecutor>(agent_), {.interceptors = {agent_.interceptor()},
                                                                  .agent_card_provider = agent_.card_provider(),
                                                                  .security_policy = agent_.security_policy(),
                                                                  .task_store = agent_.task_store(),
                                                                  .required_extensions = {}}) {}
  [[nodiscard]] auto dispatcher() const { return runtime_.dispatcher(); }
  [[nodiscard]] const auto& agent_card() const { return agent_.agent_card(); }
  void Shutdown() { runtime_.Shutdown(); }

 private:
  SutAgent agent_;
  AsyncRuntime runtime_;
};
std::unique_ptr<AsyncFixture> Agent(SutAgentOptions options = GrpcOptions()) {
  return std::make_unique<AsyncFixture>(
      Stores(), SutEndpoints{.rest_url = {}, .json_rpc_url = {}, .grpc_url = kEndpoint}, std::move(options));
}
DispatchRequest GetRequest() {
  lf::a2a::v1::GetTaskRequest get;
  get.set_id(kTaskId);
  return {.operation = DispatcherOperation::kGetTask, .payload = std::move(get)};
}
RequestContext Metadata() {
  RequestContext context;
  context.protocol_version = core::Version::kProtocolVersion;
  return context;
}
core::Result<DispatchResponse> Invoke(AsyncFixture& agent, DispatchRequest request,
                                      RequestContext metadata = Metadata()) {
  auto reply = std::make_shared<std::promise<core::Result<DispatchResponse>>>();
  auto future = reply->get_future();
  const auto admitted = agent.dispatcher()->Dispatch(
      std::move(request), std::move(metadata), [reply](auto result, auto) { reply->set_value(std::move(result)); });
  if (!admitted.ok()) {
    return admitted.error();
  }
  if (future.wait_for(kWait) != std::future_status::ready) {
    ADD_FAILURE() << kTimeout;
    return core::Error::Internal(kTimeout);
  }
  return future.get();
}

TEST(AsyncSutTest, AdvertisesEnabledBindingsAndCoroutineStreaming) {
  const auto agent = Agent();
  const auto& card = agent->agent_card();
  ASSERT_EQ(card.supported_interfaces_size(), kSingleBinding);
  EXPECT_EQ(card.supported_interfaces(0).protocol_binding(), core::protocol_bindings::kGrpc);
  EXPECT_EQ(card.supported_interfaces(0).url(), kEndpoint);
  EXPECT_TRUE(card.capabilities().streaming());
  EXPECT_TRUE(card.capabilities().push_notifications());
}

TEST(AsyncSutTest, AppliesSharedAuthorizationPolicyBeforeApplicationExecution) {
  auto options = GrpcOptions();
  options.auth_token = kToken;
  auto agent = Agent(options);
  auto missing = Invoke(*agent, GetRequest());
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().http_status(), core::http::kStatusUnauthorized);
  auto context = Metadata();
  context.auth_metadata[kBearerToken] = kToken;
  auto absent = Invoke(*agent, GetRequest(), context);
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(absent.error().protocol_code(), core::protocol_codes::kTaskNotFound);
}

TEST(AsyncSutTest, UsesSharedExtendedCardProviderWithoutNestedDispatch) {
  auto agent = Agent();
  auto result = Invoke(*agent, {.operation = DispatcherOperation::kGetExtendedAgentCard,
                                .payload = lf::a2a::v1::GetExtendedAgentCardRequest{}});
  ASSERT_TRUE(result.ok());
  const auto* card = std::get_if<lf::a2a::v1::AgentCard>(&result.value().payload());
  ASSERT_NE(card, nullptr);
  ASSERT_EQ(card->supported_interfaces_size(), kSingleBinding);
  EXPECT_EQ(card->supported_interfaces(0).protocol_binding(), core::protocol_bindings::kGrpc);
}

class Gate final {
 public:
  void Wait() {
    std::unique_lock lock(mutex_);
    threads_.insert(std::this_thread::get_id());
    ready_.notify_all();
    ready_.wait(lock, [this] { return open_; });
  }
  bool WaitForWorkers(std::size_t count) {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, kWait, [this, count] { return threads_.size() == count; });
  }
  void Open() {
    {
      std::lock_guard lock(mutex_);
      open_ = true;
    }
    ready_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::set<std::thread::id> threads_;
  bool open_ = false;
};
struct ReleaseGate final {
  std::shared_ptr<Gate> gate;
  ~ReleaseGate() { gate->Open(); }
};
class PendingReadStore final : public TaskStore {
 public:
  explicit PendingReadStore(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
  core::Result<void> CreateOrUpdate([[maybe_unused]] const lf::a2a::v1::Task& task) override { return {}; }
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Get([[maybe_unused]] std::string_view id) const override {
    gate_->Wait();
    return core::protocol_errors::TaskNotFound();
  }
  [[nodiscard]] HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override { return {}; }
  [[nodiscard]] core::Result<ListTasksResponse> List([[maybe_unused]] const ListTasksRequest& request) const override {
    return ListTasksResponse{};
  }
  core::Result<lf::a2a::v1::Task> Cancel([[maybe_unused]] std::string_view id) override {
    return core::protocol_errors::TaskNotFound();
  }
  core::Result<lf::a2a::v1::Task> AppendTaskHistory([[maybe_unused]] std::string_view id,
                                                    [[maybe_unused]] const lf::a2a::v1::Message& message,
                                                    [[maybe_unused]] HistoryAppendPolicy policy) override {
    return core::protocol_errors::TaskNotFound();
  }

 private:
  std::shared_ptr<Gate> gate_;
};

void ExpectShutdownReplies(std::vector<std::future<RequestOutcome>>& replies) {
  for (auto& reply : replies) {
    ASSERT_EQ(reply.wait_for(kWait), std::future_status::ready);
    EXPECT_EQ(reply.get(), RequestOutcome::kServerShutdown);
  }
}

TEST(AsyncSutTest, BoundedBackendWorkLeavesDispatcherAvailableForCardsAndShutdown) {
  auto gate = std::make_shared<Gate>();
  auto stores = Stores();
  stores.task_store = std::make_unique<PendingReadStore>(gate);
  AsyncFixture agent(std::move(stores), SutEndpoints{.rest_url = {}, .json_rpc_url = {}, .grpc_url = kEndpoint},
                     GrpcOptions());
  const ReleaseGate release{gate};
  std::vector<std::future<RequestOutcome>> replies;
  for (std::size_t index = 0; index < kCalls; ++index) {
    auto reply = std::make_shared<std::promise<RequestOutcome>>();
    replies.push_back(reply->get_future());
    EXPECT_TRUE(
        agent.dispatcher()
            ->Dispatch(GetRequest(), Metadata(), [reply](auto, auto context) { reply->set_value(context.Outcome()); })
            .ok());
  }
  ASSERT_TRUE(gate->WaitForWorkers(BlockingExecutorOptions::kDefaultWorkers));
  EXPECT_TRUE(Invoke(agent, {.operation = DispatcherOperation::kGetExtendedAgentCard,
                             .payload = lf::a2a::v1::GetExtendedAgentCardRequest{}})
                  .ok());
  agent.Shutdown();
  ExpectShutdownReplies(replies);
}

}  // namespace
