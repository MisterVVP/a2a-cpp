// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/core/async_operation.h"
#include "a2a/core/protocol_methods.h"

namespace {
using namespace a2a;
using client::ListTasksRequest;
using client::ListTasksResponse;
constexpr auto kWait = std::chrono::seconds(5);
constexpr auto kTimeout = std::chrono::milliseconds(10);
constexpr auto kCancelled = "CANCELLED";
constexpr auto kDeadline = "DEADLINE_EXCEEDED";
constexpr auto kFailure = "test error";
constexpr auto kBaseUrl = "https://async.example.test";
constexpr auto kCard =
    R"({"name":"async-agent","version":"1.0","supportedInterfaces":[{"protocolBinding":"HTTP+JSON","protocolVersion":"1.0","url":"https://async.example.test"}]})";
constexpr int kOk = 200;
constexpr int kDestroyCalls = 2;
constexpr int kBeforeEvent = 1;
constexpr int kAfterEvent = 2;
constexpr int kCompletionEvent = 3;
constexpr int kRaceCount = 50;
constexpr int kRaceParticipants = 3;
constexpr std::size_t kCapacity = 4096;

void ExpectBackgroundCallback(std::thread::id caller) { EXPECT_NE(std::this_thread::get_id(), caller); }

class FakeTransport final : public client::ClientTransport {
 public:
  bool hold = false;
  std::function<void()> on_shutdown;
  core::Result<void> Shutdown() override {
    if (on_shutdown) {
      on_shutdown();
    }
    return ClientTransport::Shutdown();
  }
  std::optional<core::AsyncCompletion<lf::a2a::v1::Task>> pending;
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                             const client::CallOptions& options) override {
    (void)request;
    (void)options;
    return lf::a2a::v1::SendMessageResponse{};
  }
  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          const client::CallOptions& options) override {
    (void)request;
    (void)options;
    return lf::a2a::v1::Task{};
  }
  core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                            const client::CallOptions& options) override {
    (void)request;
    (void)options;
    return ListTasksResponse{};
  }
  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             const client::CallOptions& options) override {
    (void)request;
    (void)options;
    return lf::a2a::v1::Task{};
  }
  core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const client::CallOptions& options) override {
    (void)request;
    (void)options;
    return lf::a2a::v1::TaskPushNotificationConfig{};
  }
  core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const client::CallOptions& options) override {
    (void)request;
    (void)options;
    return lf::a2a::v1::TaskPushNotificationConfig{};
  }
  core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const client::CallOptions& options) override {
    (void)request;
    (void)options;
    return lf::a2a::v1::ListTaskPushNotificationConfigsResponse{};
  }
  core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const client::CallOptions& options) override {
    (void)request;
    (void)options;
    return {};
  }
  core::Result<std::unique_ptr<client::StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, client::StreamObserver& observer,
      const client::CallOptions& options) override {
    (void)request;
    (void)observer;
    (void)options;
    return core::Error::Internal(kFailure);
  }
  core::Result<std::unique_ptr<client::StreamHandle>> SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                    client::StreamObserver& observer,
                                                                    const client::CallOptions& options) override {
    (void)request;
    (void)observer;
    (void)options;
    return core::Error::Internal(kFailure);
  }

 protected:
  void StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const client::CallOptions& options,
                        const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) override {
    (void)request;
    (void)options;
    completion.Complete(lf::a2a::v1::SendMessageResponse{});
  }
  void StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const client::CallOptions& options,
                    const core::AsyncCompletion<lf::a2a::v1::Task>& completion) override {
    (void)request;
    (void)options;
    if (hold) {
      pending = completion;
      return;
    }
    completion.Complete(lf::a2a::v1::Task{});
  }
  void StartListTasks(const ListTasksRequest& request, const client::CallOptions& options,
                      const core::AsyncCompletion<ListTasksResponse>& completion) override {
    (void)request;
    (void)options;
    completion.Complete(ListTasksResponse{});
  }
  void StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const client::CallOptions& options,
                       const core::AsyncCompletion<lf::a2a::v1::Task>& completion) override {
    (void)request;
    (void)options;
    completion.Complete(lf::a2a::v1::Task{});
  }
  void StartCreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const client::CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) override {
    (void)request;
    (void)options;
    completion.Complete(lf::a2a::v1::TaskPushNotificationConfig{});
  }
  void StartGetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const client::CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) override {
    (void)request;
    (void)options;
    completion.Complete(lf::a2a::v1::TaskPushNotificationConfig{});
  }
  void StartListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const client::CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) override {
    (void)request;
    (void)options;
    completion.Complete(lf::a2a::v1::ListTaskPushNotificationConfigsResponse{});
  }
  void StartDeleteTaskPushNotificationConfig(const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
                                             const client::CallOptions& options,
                                             const core::AsyncCompletion<void>& completion) override {
    (void)request;
    (void)options;
    completion.Complete({});
  }
};

TEST(AsyncClientContractTest, SendMessageCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<lf::a2a::v1::SendMessageResponse>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started = client.SendMessageAsync(lf::a2a::v1::SendMessageRequest{},
                                         [&](core::Result<lf::a2a::v1::SendMessageResponse> response) {
                                           ExpectBackgroundCallback(caller);
                                           ++calls;
                                           result.set_value(std::move(response));
                                         });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  started.value().Wait();
  EXPECT_FALSE(started.value().IsActive());
  EXPECT_EQ(calls.load(), 1);
}

TEST(AsyncClientContractTest, GetTaskCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started = client.GetTaskAsync(lf::a2a::v1::GetTaskRequest{}, [&](core::Result<lf::a2a::v1::Task> response) {
    ExpectBackgroundCallback(caller);
    ++calls;
    result.set_value(std::move(response));
  });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  started.value().Wait();
  EXPECT_FALSE(started.value().IsActive());
  EXPECT_EQ(calls.load(), 1);
}

TEST(AsyncClientContractTest, ListTasksCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<ListTasksResponse>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started = client.ListTasksAsync(ListTasksRequest{}, [&](core::Result<ListTasksResponse> response) {
    ExpectBackgroundCallback(caller);
    ++calls;
    result.set_value(std::move(response));
  });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  started.value().Wait();
  EXPECT_FALSE(started.value().IsActive());
  EXPECT_EQ(calls.load(), 1);
}

TEST(AsyncClientContractTest, CancelTaskCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started =
      client.CancelTaskAsync(lf::a2a::v1::CancelTaskRequest{}, [&](core::Result<lf::a2a::v1::Task> response) {
        ExpectBackgroundCallback(caller);
        ++calls;
        result.set_value(std::move(response));
      });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  started.value().Wait();
  EXPECT_FALSE(started.value().IsActive());
  EXPECT_EQ(calls.load(), 1);
}

TEST(AsyncClientContractTest, CreateTaskPushNotificationConfigCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started = client.CreateTaskPushNotificationConfigAsync(
      lf::a2a::v1::TaskPushNotificationConfig{}, [&](core::Result<lf::a2a::v1::TaskPushNotificationConfig> response) {
        ExpectBackgroundCallback(caller);
        ++calls;
        result.set_value(std::move(response));
      });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  started.value().Wait();
  EXPECT_FALSE(started.value().IsActive());
  EXPECT_EQ(calls.load(), 1);
}

TEST(AsyncClientContractTest, GetTaskPushNotificationConfigCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started =
      client.GetTaskPushNotificationConfigAsync(lf::a2a::v1::GetTaskPushNotificationConfigRequest{},
                                                [&](core::Result<lf::a2a::v1::TaskPushNotificationConfig> response) {
                                                  ExpectBackgroundCallback(caller);
                                                  ++calls;
                                                  result.set_value(std::move(response));
                                                });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  started.value().Wait();
  EXPECT_FALSE(started.value().IsActive());
  EXPECT_EQ(calls.load(), 1);
}

TEST(AsyncClientContractTest, ListTaskPushNotificationConfigsCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started = client.ListTaskPushNotificationConfigsAsync(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest{},
      [&](core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> response) {
        ExpectBackgroundCallback(caller);
        ++calls;
        result.set_value(std::move(response));
      });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  started.value().Wait();
  EXPECT_FALSE(started.value().IsActive());
  EXPECT_EQ(calls.load(), 1);
}

TEST(AsyncClientContractTest, DeleteTaskPushNotificationConfigCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<void>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started = client.DeleteTaskPushNotificationConfigAsync(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest{},
                                                              [&](core::Result<void> response) {
                                                                ExpectBackgroundCallback(caller);
                                                                ++calls;
                                                                result.set_value(std::move(response));
                                                              });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(future.get().ok());
  started.value().Wait();
  EXPECT_FALSE(started.value().IsActive());
  EXPECT_EQ(calls.load(), 1);
}

TEST(AsyncClientContractTest, CancellationPropagatesBeforeNetworkRegistration) {
  core::AsyncScope scope;
  std::promise<core::Result<int>> result;
  auto future = result.get_future();
  auto token = scope.Start<int>([&](core::Result<int> value) { result.set_value(std::move(value)); }, kWait);
  ASSERT_TRUE(token.ok());
  token.value().handle().Cancel();
  int cancellations = 0;
  token.value().RegisterCancellation([&] { ++cancellations; });
  token.value().Complete(1);
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kCancelled);
  EXPECT_EQ(cancellations, 1);
}

TEST(AsyncClientContractTest, NonpositiveDeadlineSkipsTransportInitiation) {
  auto transport = std::make_unique<FakeTransport>();
  auto* fake = transport.get();
  fake->hold = true;
  client::A2AClient client(std::move(transport));
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  client::CallOptions options;
  options.timeout = std::chrono::milliseconds::zero();
  auto started = client.GetTaskAsync({}, [&](auto value) { result.set_value(std::move(value)); }, options);
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kDeadline);
  EXPECT_FALSE(fake->pending.has_value());
  started.value().Wait();
}

TEST(AsyncClientContractTest, DeadlineWinsAgainstLateTransportResult) {
  auto transport = std::make_unique<FakeTransport>();
  auto* fake = transport.get();
  fake->hold = true;
  client::A2AClient client(std::move(transport));
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  client::CallOptions options;
  options.timeout = kTimeout;
  auto started = client.GetTaskAsync({}, [&](auto value) { result.set_value(std::move(value)); }, options);
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kDeadline);
  if (fake->pending.has_value()) {
    fake->pending->Complete(lf::a2a::v1::Task{});
  }
  started.value().Wait();
}

TEST(AsyncClientContractTest, CompletionCancelAndShutdownRaceOnce) {
  for (int index = 0; index < kRaceCount; ++index) {
    core::AsyncScope scope;
    std::atomic<int> calls{0};
    auto token = scope.Start<int>(
        [&](auto result) {
          (void)result;
          ++calls;
        },
        kWait);
    ASSERT_TRUE(token.ok());
    std::barrier gate(kRaceParticipants);
    std::thread complete([&] {
      gate.arrive_and_wait();
      token.value().Complete(1);
    });
    std::thread cancel([&] {
      gate.arrive_and_wait();
      token.value().handle().Cancel();
    });
    gate.arrive_and_wait();
    scope.Shutdown();
    complete.join();
    cancel.join();
    token.value().handle().Wait();
    EXPECT_EQ(calls.load(), 1);
  }
}

class RecordingInterceptor final : public client::ClientInterceptor {
 public:
  std::vector<int>& order;
  explicit RecordingInterceptor(std::vector<int>& events) : order(events) {}
  void BeforeCall(const client::ClientCallContext& context) override {
    EXPECT_EQ(context.operation, core::protocol_methods::kGetTask);
    order.push_back(kBeforeEvent);
  }
  void AfterCall(const client::ClientCallContext& context, const client::ClientCallResult& result) override {
    EXPECT_EQ(context.operation, core::protocol_methods::kGetTask);
    EXPECT_FALSE(result.ok);
    order.push_back(kAfterEvent);
  }
};

TEST(AsyncClientContractTest, DestroyDrainsInterceptorsBeforeCompletionAndRejectsNewCalls) {
  auto transport = std::make_unique<FakeTransport>();
  transport->hold = true;
  client::A2AClient client(std::move(transport));
  std::vector<int> order;
  client.AddInterceptor(std::make_shared<RecordingInterceptor>(order));
  auto started = client.GetTaskAsync({}, [&](auto result) {
    EXPECT_FALSE(result.ok());
    order.push_back(kCompletionEvent);
  });
  ASSERT_TRUE(started.ok());
  EXPECT_TRUE(client.Destroy().ok());
  EXPECT_EQ(order, (std::vector<int>{kBeforeEvent, kAfterEvent, kCompletionEvent}));
  EXPECT_FALSE(client.GetTaskAsync({}, [](auto result) { (void)result; }).ok());
}

TEST(AsyncClientContractTest, ConcurrentDestroyCallsBothDrainTheSameCallbacks) {
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();
  std::promise<void> both_shutdowns;
  auto both_future = both_shutdowns.get_future();
  std::atomic<int> shutdowns{0};
  auto transport = std::make_unique<FakeTransport>();
  transport->on_shutdown = [&] {
    if (++shutdowns == kDestroyCalls) {
      both_shutdowns.set_value();
    }
  };
  client::A2AClient client(std::move(transport));
  auto operation = client.GetTaskAsync({}, [&](auto result) {
    (void)result;
    entered.set_value();
    release_future.wait();
  });
  ASSERT_TRUE(operation.ok());
  ASSERT_EQ(entered_future.wait_for(kWait), std::future_status::ready);
  core::Result<void> first_result;
  core::Result<void> second_result;
  std::thread first([&] { first_result = client.Destroy(); });
  std::thread second([&] { second_result = client.Destroy(); });
  const bool shared_shutdown = both_future.wait_for(kWait) == std::future_status::ready;
  release.set_value();
  first.join();
  second.join();
  EXPECT_TRUE(shared_shutdown);
  EXPECT_TRUE(first_result.ok());
  EXPECT_TRUE(second_result.ok());
  operation.value().Wait();
}

TEST(AsyncClientContractTest, CompletionCanDestroyClientAndThrow) {
  auto client = std::make_unique<client::A2AClient>(std::make_unique<FakeTransport>());
  std::promise<void> done;
  auto future = done.get_future();
  auto started = client->GetTaskAsync({}, [&](auto result) {
    EXPECT_TRUE(result.ok());
    client.reset();
    done.set_value();
    throw std::runtime_error(kFailure);
  });
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  started.value().Wait();
}

TEST(AsyncClientContractTest, DestructionContainsCustomShutdownExceptions) {
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  auto transport = std::make_unique<FakeTransport>();
  transport->hold = true;
  transport->on_shutdown = [] { throw std::runtime_error(kFailure); };
  auto client = std::make_unique<client::A2AClient>(std::move(transport));
  auto operation = client->GetTaskAsync({}, [&](auto value) { result.set_value(std::move(value)); });
  ASSERT_TRUE(operation.ok());
  client.reset();
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  EXPECT_FALSE(future.get().ok());
  operation.value().Wait();
}

TEST(AsyncClientContractTest, CapacityIncludesPendingCallbacksAndRecovers) {
  core::AsyncScope scope;
  std::vector<core::AsyncCompletion<int>> pending;
  pending.reserve(kCapacity);
  for (std::size_t index = 0; index < kCapacity; ++index) {
    auto token = scope.Start<int>([](auto result) { (void)result; }, kWait);
    ASSERT_TRUE(token.ok());
    pending.push_back(std::move(token.value()));
  }
  EXPECT_FALSE(scope.Start<int>([](auto result) { (void)result; }, kWait).ok());
  for (const auto& token : pending) {
    token.Complete(1);
    token.handle().Wait();
  }
  EXPECT_TRUE(scope.Start<int>([](auto result) { (void)result; }, kWait).ok());
}

TEST(AsyncClientContractTest, ThrowingCancellationSignalDoesNotStopDeadlineThread) {
  core::AsyncScope scope;
  std::promise<core::Result<int>> result;
  auto future = result.get_future();
  auto token = scope.Start<int>([&](auto value) { result.set_value(std::move(value)); }, kTimeout);
  ASSERT_TRUE(token.ok());
  token.value().RegisterCancellation([] { throw std::runtime_error(kFailure); });
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kDeadline);
  token.value().handle().Wait();
}

TEST(AsyncClientContractTest, StreamCallbackShutdownDoesNotWaitOnUnaryCallback) {
  core::AsyncScope scope;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();
  auto token = scope.Start<int>(
      [&](auto result) {
        (void)result;
        entered.set_value();
        release_future.wait();
      },
      kWait);
  ASSERT_TRUE(token.ok());
  token.value().Complete(1);
  ASSERT_EQ(entered_future.wait_for(kWait), std::future_status::ready);
  {
    client::StreamHandle::State stream_state;
    const client::StreamHandle::State::CallbackExecutionScope callback_scope(stream_state);
    scope.Shutdown();
    token.value().handle().Wait();
  }
  release.set_value();
  token.value().handle().Wait();
}

TEST(AsyncClientContractTest, MissingCallbackAndClosedScopeRejectWithoutCompletion) {
  core::AsyncScope scope;
  EXPECT_FALSE(scope.Start<int>({}, kWait).ok());
  scope.Shutdown();
  EXPECT_FALSE(scope.Start<int>([](auto result) { (void)result; }, kWait).ok());
}

client::HttpAsyncFetcher ImmediateFetcher(core::AsyncScope& network, int& requests) {
  return
      [&](http::Request request, core::Completion<client::HttpResponse> completion) -> core::Result<core::AsyncHandle> {
        EXPECT_FALSE(request.headers.empty());
        ++requests;
        auto token = network.Start<client::HttpResponse>(std::move(completion), kWait);
        if (!token.ok()) {
          return token.error();
        }
        token.value().Complete(client::HttpResponse{.status_code = kOk, .body = kCard});
        return token.value().handle();
      };
}
core::Result<lf::a2a::v1::AgentCard> FetchCard(client::DiscoveryClient& discovery, bool extended = false) {
  std::promise<core::Result<lf::a2a::v1::AgentCard>> result;
  auto future = result.get_future();
  const auto complete = [&](auto value) { result.set_value(std::move(value)); };
  auto started =
      extended ? discovery.FetchExtendedAgentCardAsync(kBaseUrl, complete) : discovery.FetchAsync(kBaseUrl, complete);
  if (!started.ok()) {
    return started.error();
  }
  if (future.wait_for(kWait) != std::future_status::ready) {
    started.value().Cancel();
    started.value().Wait();
    return core::Error::Internal(kFailure);
  }
  auto response = future.get();
  started.value().Wait();
  return response;
}
TEST(AsyncDiscoveryTest, FetchVerifiesCachesAndSupportsExtendedDiscovery) {
  core::AsyncScope network;
  int requests = 0;
  client::DiscoveryClient discovery({}, client::kDefaultDiscoveryCacheTtl, ImmediateFetcher(network, requests));
  EXPECT_TRUE(FetchCard(discovery).ok());
  EXPECT_TRUE(FetchCard(discovery).ok());
  EXPECT_EQ(requests, 1);
  discovery.SetAgentCardVerifier([](auto json) -> core::Result<void> {
    (void)json;
    return core::Error::Validation(kFailure);
  });
  EXPECT_FALSE(FetchCard(discovery, true).ok());
  discovery.Shutdown();
}
}  // namespace
