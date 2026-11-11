// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/async_client.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "a2a/client/async_transport.h"
#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/core/async_operation.h"
#include "a2a/core/protocol_methods.h"

namespace {
using namespace a2a;
using client::ListTasksRequest;
using client::ListTasksResponse;
// Owning clients/scopes have stable identities; callback facades are value views.
static_assert(!std::is_copy_constructible_v<client::A2AClient>);
static_assert(!std::is_copy_assignable_v<client::A2AClient>);
static_assert(!std::is_move_constructible_v<client::A2AClient>);
static_assert(!std::is_move_assignable_v<client::A2AClient>);
static_assert(std::is_copy_constructible_v<client::AsyncClient>);
static_assert(std::is_copy_assignable_v<client::AsyncClient>);
static_assert(std::is_move_constructible_v<client::AsyncClient>);
static_assert(std::is_move_assignable_v<client::AsyncClient>);
static_assert(!std::is_copy_constructible_v<client::AsyncClientTransport>);
static_assert(!std::is_move_constructible_v<client::AsyncClientTransport>);
static_assert(!std::is_copy_constructible_v<core::AsyncScope>);
static_assert(!std::is_move_constructible_v<core::AsyncScope>);
constexpr auto kWait = std::chrono::seconds(5);
constexpr auto kTimeout = std::chrono::milliseconds(10);
constexpr auto kVerifierWait = std::chrono::seconds(1);
constexpr std::size_t kDeadlineBatch = 64;
constexpr int kCardRefetchRequests = 2;
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
constexpr int kCompletedValue = 42;

void ExpectBackgroundCallback(std::thread::id caller) { EXPECT_NE(std::this_thread::get_id(), caller); }

class FakeAsyncTransport final : public client::AsyncClientTransport {
 public:
  bool hold = false;
  std::optional<core::AsyncCompletion<lf::a2a::v1::Task>> pending;

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
class FakeTransport final : public client::ClientTransport {
 public:
  std::function<void()> on_shutdown;
  std::function<void()> on_async;
  core::Result<void> Shutdown() override {
    if (on_shutdown) {
      on_shutdown();
    }
    async->Shutdown();
    return {};
  }
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

  std::shared_ptr<FakeAsyncTransport> async = std::make_shared<FakeAsyncTransport>();
  [[nodiscard]] std::shared_ptr<client::AsyncClientTransport> Async() const override {
    if (on_async) {
      on_async();
    }
    return async;
  }
};

TEST(AsyncClientContractTest, SendMessageCompletesOnceAwayFromCaller) {
  client::A2AClient client(std::make_unique<FakeTransport>());
  std::promise<core::Result<lf::a2a::v1::SendMessageResponse>> result;
  auto future = result.get_future();
  const auto caller = std::this_thread::get_id();
  std::atomic<int> calls{0};
  auto started = client.Async().SendMessage(lf::a2a::v1::SendMessageRequest{},
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
  auto started = client.Async().GetTask(lf::a2a::v1::GetTaskRequest{}, [&](core::Result<lf::a2a::v1::Task> response) {
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
  auto started = client.Async().ListTasks(ListTasksRequest{}, [&](core::Result<ListTasksResponse> response) {
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
      client.Async().CancelTask(lf::a2a::v1::CancelTaskRequest{}, [&](core::Result<lf::a2a::v1::Task> response) {
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
  auto started = client.Async().CreateTaskPushNotificationConfig(
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
      client.Async().GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest{},
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
  auto started = client.Async().ListTaskPushNotificationConfigs(
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
  auto started = client.Async().DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest{},
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

void ExpectCancelledTask(std::future<core::Result<lf::a2a::v1::Task>>& future) {
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto result = future.get();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), kCancelled);
}

TEST(AsyncClientContractTest, FacadeCopiesObserveOwnerDestructionAndCancelPendingCalls) {
  auto transport = std::make_unique<FakeTransport>();
  transport->async->hold = true;
  auto owner = std::make_unique<client::A2AClient>(std::move(transport));
  auto view = owner->Async();
  auto copied_view = view;
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  auto operation = copied_view.GetTask({}, [&](auto value) { result.set_value(std::move(value)); });
  ASSERT_TRUE(operation.ok());
  owner.reset();
  ExpectCancelledTask(future);
  operation.value().Wait();
  EXPECT_FALSE(view.GetTask({}, [](auto value) { (void)value; }).ok());
  EXPECT_FALSE(copied_view.GetTask({}, [](auto value) { (void)value; }).ok());
}

class CountingInterceptor final : public client::ClientInterceptor {
 public:
  int before = 0;
  int after = 0;
  void BeforeCall(const client::ClientCallContext& context) override {
    (void)context;
    ++before;
  }
  void AfterCall(const client::ClientCallContext& context, const client::ClientCallResult& result) override {
    (void)context;
    (void)result;
    ++after;
  }
};
class RegisteringInterceptor final : public client::ClientInterceptor {
 public:
  RegisteringInterceptor(client::A2AClient& owner, std::shared_ptr<CountingInterceptor> next)
      : owner_(owner), next_(std::move(next)) {}
  void BeforeCall(const client::ClientCallContext& context) override {
    (void)context;
    if (!std::exchange(registered_, true)) {
      owner_.AddInterceptor(next_);
    }
  }

 private:
  client::A2AClient& owner_;
  std::shared_ptr<CountingInterceptor> next_;
  bool registered_ = false;
};
void ExpectCallCounts(const CountingInterceptor& interceptor, int expected) {
  EXPECT_EQ(interceptor.before, expected);
  EXPECT_EQ(interceptor.after, expected);
}
TEST(AsyncClientContractTest, InterceptorCanRegisterAnotherWithoutDeadlockOrUnpairedAfter) {
  client::A2AClient owner(std::make_unique<FakeTransport>());
  auto counter = std::make_shared<CountingInterceptor>();
  owner.AddInterceptor(std::make_shared<RegisteringInterceptor>(owner, counter));
  EXPECT_TRUE(owner.GetTask({}).ok());
  ExpectCallCounts(*counter, 0);
  auto operation = owner.Async().GetTask({}, [](auto value) { (void)value; });
  ASSERT_TRUE(operation.ok());
  operation.value().Wait();
  ExpectCallCounts(*counter, 1);
}
TEST(AsyncClientContractTest, ThrowingAsyncProviderPairsInterceptorsWithoutCallingCompletion) {
  auto transport = std::make_unique<FakeTransport>();
  auto* fake = transport.get();
  client::A2AClient owner(std::move(transport));
  auto counter = std::make_shared<CountingInterceptor>();
  owner.AddInterceptor(counter);
  fake->on_async = [] { throw std::runtime_error(kFailure); };
  bool callback_called = false;
  auto operation = owner.Async().GetTask({}, [&](auto value) {
    (void)value;
    callback_called = true;
  });
  fake->on_async = {};
  EXPECT_FALSE(operation.ok());
  EXPECT_FALSE(callback_called);
  ExpectCallCounts(*counter, 1);
}

TEST(AsyncClientContractTest, FacadeCanBeMovedAndReassigned) {
  client::A2AClient owner(std::make_unique<FakeTransport>());
  auto original = owner.Async();
  auto moved = std::move(original);
  original = owner.Async();
  auto operation = moved.GetTask({}, [](auto value) { (void)value; });
  ASSERT_TRUE(operation.ok());
  operation.value().Wait();
  auto reassigned = original.GetTask({}, [](auto value) { (void)value; });
  ASSERT_TRUE(reassigned.ok());
  reassigned.value().Wait();
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
  fake->async->hold = true;
  client::A2AClient client(std::move(transport));
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  client::CallOptions options;
  options.timeout = std::chrono::milliseconds::zero();
  auto started = client.Async().GetTask({}, [&](auto value) { result.set_value(std::move(value)); }, options);
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kDeadline);
  EXPECT_FALSE(fake->async->pending.has_value());
  started.value().Wait();
}

TEST(AsyncClientContractTest, DeadlineWinsAgainstLateTransportResult) {
  auto transport = std::make_unique<FakeTransport>();
  auto* fake = transport.get();
  fake->async->hold = true;
  client::A2AClient client(std::move(transport));
  std::promise<core::Result<lf::a2a::v1::Task>> result;
  auto future = result.get_future();
  client::CallOptions options;
  options.timeout = kTimeout;
  auto started = client.Async().GetTask({}, [&](auto value) { result.set_value(std::move(value)); }, options);
  ASSERT_TRUE(started.ok());
  ASSERT_EQ(future.wait_for(kWait), std::future_status::ready);
  auto response = future.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kDeadline);
  auto& pending = fake->async->pending;
  if (pending.has_value()) {
    pending->Complete(lf::a2a::v1::Task{});
  }
  started.value().Wait();
}

void ExpectFiniteDeadline(core::AsyncScope& scope) {
  std::promise<core::Result<int>> expired;
  auto expiry = expired.get_future();
  auto finite = scope.Start<int>([&](auto value) { expired.set_value(std::move(value)); }, kTimeout);
  ASSERT_TRUE(finite.ok());
  ASSERT_EQ(expiry.wait_for(kWait), std::future_status::ready);
  const auto response = expiry.get();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().protocol_code(), kDeadline);
  finite.value().handle().Wait();
}

TEST(AsyncClientContractTest, UnboundedDeadlineAllowsCompletionAndLaterExpiry) {
  core::AsyncScope scope;
  std::promise<core::Result<int>> completed;
  auto completion = completed.get_future();
  auto unbounded =
      scope.Start<int>([&](auto value) { completed.set_value(std::move(value)); }, std::chrono::milliseconds::max());
  ASSERT_TRUE(unbounded.ok());
  unbounded.value().Complete(kCompletedValue);
  ASSERT_EQ(completion.wait_for(kWait), std::future_status::ready);
  ASSERT_EQ(completion.get().value(), kCompletedValue);
  unbounded.value().handle().Wait();
  ExpectFiniteDeadline(scope);
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
  transport->async->hold = true;
  client::A2AClient client(std::move(transport));
  std::vector<int> order;
  client.AddInterceptor(std::make_shared<RecordingInterceptor>(order));
  auto started = client.Async().GetTask({}, [&](auto result) {
    EXPECT_FALSE(result.ok());
    order.push_back(kCompletionEvent);
  });
  ASSERT_TRUE(started.ok());
  EXPECT_TRUE(client.Destroy().ok());
  EXPECT_EQ(order, (std::vector<int>{kBeforeEvent, kAfterEvent, kCompletionEvent}));
  EXPECT_FALSE(client.Async().GetTask({}, [](auto result) { (void)result; }).ok());
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
  auto operation = client.Async().GetTask({}, [&](auto result) {
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
  auto started = client->Async().GetTask({}, [&](auto result) {
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
  transport->async->hold = true;
  transport->on_shutdown = [] { throw std::runtime_error(kFailure); };
  auto client = std::make_unique<client::A2AClient>(std::move(transport));
  auto operation = client->Async().GetTask({}, [&](auto value) { result.set_value(std::move(value)); });
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

void ExpectDeadline(const core::Result<int>& result) {
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), kDeadline);
}
TEST(AsyncClientContractTest, ExpiredBatchCompletesOnceAndReleasesAdmission) {
  std::atomic<std::size_t> expired{0};
  core::AsyncScope scope;
  std::vector<core::AsyncHandle> handles;
  handles.reserve(kDeadlineBatch);
  for (std::size_t index = 0; index < kDeadlineBatch; ++index) {
    auto token = scope.Start<int>(
        [&expired](auto result) {
          ExpectDeadline(result);
          ++expired;
        },
        kTimeout);
    ASSERT_TRUE(token.ok());
    handles.push_back(token.value().handle());
  }
  for (const auto& handle : handles) {
    handle.Wait();
  }
  EXPECT_EQ(expired.load(), kDeadlineBatch);
  auto recovered = scope.Start<int>([](auto result) { (void)result; }, kWait);
  ASSERT_TRUE(recovered.ok());
  recovered.value().Complete(1);
  recovered.value().handle().Wait();
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
core::Result<lf::a2a::v1::AgentCard> FetchCard(client::DiscoveryClient& discovery, bool extended = false,
                                               std::chrono::milliseconds wait = kWait) {
  std::promise<core::Result<lf::a2a::v1::AgentCard>> result;
  auto future = result.get_future();
  const auto complete = [&](auto value) { result.set_value(std::move(value)); };
  auto started =
      extended ? discovery.FetchExtendedAgentCardAsync(kBaseUrl, complete) : discovery.FetchAsync(kBaseUrl, complete);
  if (!started.ok()) {
    return started.error();
  }
  if (future.wait_for(wait) != std::future_status::ready) {
    started.value().Cancel();
  }
  started.value().Wait();
  auto response = future.get();
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
void ExpectInternalCardError(const core::Result<lf::a2a::v1::AgentCard>& result) {
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kInternal);
}
TEST(AsyncDiscoveryTest, ThrowingVerifierCompletesWithInternalErrorWithoutWaitingForDeadline) {
  core::AsyncScope network;
  int requests = 0;
  client::DiscoveryClient discovery({}, client::kDefaultDiscoveryCacheTtl, ImmediateFetcher(network, requests));
  discovery.SetAgentCardVerifier([](auto json) -> core::Result<void> {
    (void)json;
    throw std::runtime_error(kFailure);
  });
  ExpectInternalCardError(FetchCard(discovery, false, kVerifierWait));
  EXPECT_EQ(requests, 1);
}
TEST(AsyncDiscoveryTest, CacheHitRevalidatesWhenVerifierStartsThrowing) {
  core::AsyncScope network;
  int requests = 0;
  bool fail_verification = false;
  client::DiscoveryClient discovery({}, client::kDefaultDiscoveryCacheTtl, ImmediateFetcher(network, requests));
  discovery.SetAgentCardVerifier([&fail_verification](auto json) -> core::Result<void> {
    (void)json;
    if (fail_verification) {
      throw std::runtime_error(kFailure);
    }
    return {};
  });
  EXPECT_TRUE(FetchCard(discovery).ok());
  fail_verification = true;
  ExpectInternalCardError(FetchCard(discovery, false, kVerifierWait));
  EXPECT_EQ(requests, kCardRefetchRequests);
}
void ExpectDiscoveryVerificationDrain(bool destroy) {
  core::AsyncScope network;
  int requests = 0;
  auto original = std::make_unique<client::DiscoveryClient>(client::HttpFetcher{}, client::kDefaultDiscoveryCacheTtl,
                                                            ImmediateFetcher(network, requests));
  auto discovery = std::make_unique<client::DiscoveryClient>(*original);
  original.reset();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future().share();
  discovery->SetAgentCardVerifier([&entered, release_future](auto json) -> core::Result<void> {
    (void)json;
    entered.set_value();
    release_future.wait();
    return {};
  });
  std::promise<core::Result<lf::a2a::v1::AgentCard>> result;
  auto result_future = result.get_future();
  auto operation = discovery->FetchAsync(kBaseUrl, [&](auto response) { result.set_value(std::move(response)); });
  ASSERT_TRUE(operation.ok());
  const bool verifying = entered_future.wait_for(kWait) == std::future_status::ready;
  std::promise<void> stopped;
  auto stopped_future = stopped.get_future();
  std::thread shutdown([&] {
    if (destroy) {
      discovery.reset();
    } else {
      discovery->Shutdown();
    }
    stopped.set_value();
  });
  const bool completed = result_future.wait_for(kWait) == std::future_status::ready;
  const bool drained = stopped_future.wait_for(kTimeout) == std::future_status::timeout;
  release.set_value();
  shutdown.join();
  operation.value().Wait();
  EXPECT_TRUE(verifying);
  EXPECT_TRUE(completed);
  EXPECT_TRUE(drained);
}

TEST(AsyncDiscoveryTest, ShutdownWaitsForRunningVerification) { ExpectDiscoveryVerificationDrain(false); }

TEST(AsyncDiscoveryTest, DestructionOfLastCopyWaitsForRunningVerification) { ExpectDiscoveryVerificationDrain(true); }

TEST(AsyncDiscoveryTest, VerifierCanShutdownDiscoveryOnCacheHit) {
  core::AsyncScope network;
  int requests = 0;
  bool shutdown = false;
  client::DiscoveryClient discovery({}, client::kDefaultDiscoveryCacheTtl, ImmediateFetcher(network, requests));
  discovery.SetAgentCardVerifier([&](auto json) -> core::Result<void> {
    (void)json;
    if (shutdown) {
      discovery.Shutdown();
    }
    return {};
  });
  EXPECT_TRUE(FetchCard(discovery).ok());
  shutdown = true;
  auto result = FetchCard(discovery, false, kVerifierWait);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().protocol_code(), kCancelled);
  EXPECT_EQ(requests, 1);
}
}  // namespace
