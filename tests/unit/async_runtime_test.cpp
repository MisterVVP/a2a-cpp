// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_runtime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "a2a/server/engine/blocking_executor_adapter.h"
#include "support/asio_test_support.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
namespace asio = boost::asio;
constexpr auto kTask = "adapter-task";
constexpr auto kPrincipal = "adapter-principal";
constexpr auto kFailure = "adapter-backend-failure";
constexpr auto kWait = std::chrono::seconds(5);
constexpr auto kNoWait = std::chrono::seconds::zero();
constexpr std::size_t kOne = 1;
constexpr std::size_t kTwo = 2;

enum class Behavior : std::uint8_t { kSuccess, kError, kThrow, kWait };
class Gate final {
 public:
  void Enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return open_; });
  }
  bool WaitForEntry() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kWait, [this] { return entered_; });
  }
  void Open() {
    {
      std::lock_guard lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_ = false;
  bool open_ = false;
};
struct ReleaseGate final {
  std::shared_ptr<Gate> gate;
  BlockingExecutorAdapter* adapter = nullptr;
  ~ReleaseGate() {
    gate->Open();
    if (adapter != nullptr) {
      adapter->Join();
    }
  }
};
class Backend final : public AgentExecutor {
 public:
  explicit Backend(Behavior behavior = Behavior::kSuccess) : behavior_(behavior) {}
  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          RequestContext& context) override {
    ++calls;
    thread = std::this_thread::get_id();
    principal = context.principal;
    if (on_read) {
      on_read();
    }
    if (behavior_ == Behavior::kWait) {
      gate->Enter();
    }
    if (behavior_ == Behavior::kThrow) {
      throw std::runtime_error(kFailure);
    }
    if (behavior_ == Behavior::kError) {
      return core::Error::Validation(kFailure);
    }
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                             RequestContext& /*context*/) override {
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_message() = request.message();
    return response;
  }
  core::Result<std::unique_ptr<ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& /*request*/, RequestContext& /*context*/) override {
    return std::unique_ptr<ServerStreamSession>{};
  }
  core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request, RequestContext& /*context*/) override {
    ListTasksResponse response;
    response.next_page_token = request.page_token;
    return response;
  }
  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                             RequestContext& /*context*/) override {
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }
  core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, RequestContext& /*context*/) override {
    return request;
  }
  core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, RequestContext& /*context*/) override {
    lf::a2a::v1::TaskPushNotificationConfig response;
    response.set_id(request.id());
    return response;
  }
  core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, RequestContext& /*context*/) override {
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
    response.set_next_page_token(request.page_token());
    return response;
  }
  core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& /*request*/, RequestContext& /*context*/) override {
    return {};
  }
  std::function<void()> on_read;
  std::shared_ptr<Gate> gate = std::make_shared<Gate>();
  std::atomic_int calls = 0;
  std::thread::id thread;
  std::string principal;

 private:
  Behavior behavior_;
};
lf::a2a::v1::GetTaskRequest Request() {
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTask);
  return request;
}
template <typename T>
asio::awaitable<void> Capture(asio::awaitable<core::Result<T>> operation,
                              std::shared_ptr<std::promise<core::Result<T>>> completion) {
  try {
    completion->set_value(co_await std::move(operation));
  } catch (...) {
    completion->set_exception(std::current_exception());
  }
}
template <typename T>
std::future<core::Result<T>> Start(asio::io_context& loop, asio::awaitable<core::Result<T>> operation) {
  auto completion = std::make_shared<std::promise<core::Result<T>>>();
  auto result = completion->get_future();
  asio::co_spawn(loop, Capture(std::move(operation), completion), asio::detached);
  return result;
}
template <typename T>
core::Result<T> AwaitResult(asio::io_context& loop, asio::awaitable<core::Result<T>> operation) {
  auto result = Start(loop, std::move(operation));
  loop.restart();
  loop.run();
  return result.get();
}

asio::awaitable<core::Result<lf::a2a::v1::Task>> ObserveResult(BlockingExecutorAdapter& adapter,
                                                               lf::a2a::v1::GetTaskRequest request,
                                                               RequestExecutionContext context,
                                                               std::thread::id& resumed) {
  auto result = co_await adapter.GetTask(std::move(request), std::move(context));
  resumed = std::this_thread::get_id();
  co_return result;
}

TEST(BlockingExecutorAdapterTest, OwnsArgumentsAndReturnsResultOnAwaitingExecutor) {
  auto backend = std::make_shared<Backend>();
  BlockingExecutorAdapter adapter(backend);
  asio::io_context loop;
  RequestContext metadata;
  metadata.principal = kPrincipal;
  RequestExecution source(metadata);
  auto request = Request();
  std::thread::id resumed;
  auto operation = ObserveResult(adapter, request, source.Context(), resumed);
  request.clear_id();
  metadata.principal.clear();
  auto result = AwaitResult(loop, std::move(operation));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().id(), kTask);
  EXPECT_EQ(backend->principal, kPrincipal);
  EXPECT_NE(backend->thread, std::this_thread::get_id());
  EXPECT_EQ(resumed, std::this_thread::get_id());
}

TEST(BlockingExecutorAdapterTest, ForwardsMessageTaskAndListResults) {
  BlockingExecutorAdapter adapter(std::make_shared<Backend>());
  asio::io_context loop;
  RequestExecution source;
  lf::a2a::v1::SendMessageRequest send;
  send.mutable_message()->set_message_id(kTask);
  const auto sent = AwaitResult(loop, adapter.SendMessage(send, source.Context()));
  ASSERT_TRUE(sent.ok());
  EXPECT_EQ(sent.value().message().message_id(), kTask);
  const auto listed = AwaitResult(loop, adapter.ListTasks(ListTasksRequest{kOne, kTask}, source.Context()));
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value().next_page_token, kTask);
  lf::a2a::v1::CancelTaskRequest cancel;
  cancel.set_id(kTask);
  const auto cancelled = AwaitResult(loop, adapter.CancelTask(cancel, source.Context()));
  ASSERT_TRUE(cancelled.ok());
  EXPECT_EQ(cancelled.value().id(), kTask);
}

TEST(BlockingExecutorAdapterTest, ForwardsPushConfigurationResultsIncludingVoid) {
  BlockingExecutorAdapter adapter(std::make_shared<Backend>());
  asio::io_context loop;
  RequestExecution source;
  lf::a2a::v1::TaskPushNotificationConfig config;
  config.set_id(kTask);
  const auto created = AwaitResult(loop, adapter.CreateTaskPushNotificationConfig(config, source.Context()));
  ASSERT_TRUE(created.ok());
  EXPECT_EQ(created.value().id(), kTask);
  lf::a2a::v1::GetTaskPushNotificationConfigRequest get;
  get.set_id(kTask);
  const auto got = AwaitResult(loop, adapter.GetTaskPushNotificationConfig(get, source.Context()));
  ASSERT_TRUE(got.ok());
  EXPECT_EQ(got.value().id(), kTask);
  lf::a2a::v1::ListTaskPushNotificationConfigsRequest list;
  list.set_page_token(kTask);
  const auto configs = AwaitResult(loop, adapter.ListTaskPushNotificationConfigs(list, source.Context()));
  ASSERT_TRUE(configs.ok());
  EXPECT_EQ(configs.value().next_page_token(), kTask);
  EXPECT_TRUE(AwaitResult(loop, adapter.DeleteTaskPushNotificationConfig({}, source.Context())).ok());
}

TEST(BlockingExecutorAdapterTest, ReturnedAwaitableOwnsStateAfterAdapterDestruction) {
  auto backend = std::make_shared<Backend>();
  RequestExecution source;
  auto operation = [&] {
    BlockingExecutorAdapter adapter(backend);
    return adapter.GetTask(Request(), source.Context());
  }();
  asio::io_context loop;
  EXPECT_FALSE(AwaitResult(loop, std::move(operation)).ok());
  EXPECT_EQ(backend->calls.load(), 0);
}

TEST(BlockingExecutorAdapterTest, PreservesErrorsAndContainsBackendExceptions) {
  asio::io_context loop;
  RequestExecution source;
  BlockingExecutorAdapter errors(std::make_shared<Backend>(Behavior::kError));
  auto error = AwaitResult(loop, errors.GetTask(Request(), source.Context()));
  ASSERT_FALSE(error.ok());
  EXPECT_EQ(error.error().code(), core::ErrorCode::kValidation);
  EXPECT_EQ(error.error().message(), kFailure);
  BlockingExecutorAdapter exceptions(std::make_shared<Backend>(Behavior::kThrow));
  error = AwaitResult(loop, exceptions.GetTask(Request(), source.Context()));
  ASSERT_FALSE(error.ok());
  EXPECT_EQ(error.error().code(), core::ErrorCode::kInternal);
}

TEST(BlockingExecutorAdapterTest, BoundsAllPendingWorkAndClosesAdmissionWithoutWaiting) {
  auto backend = std::make_shared<Backend>(Behavior::kWait);
  BlockingExecutorAdapter adapter(backend, {.workers = kOne, .max_pending = kTwo});
  asio::io_context loop;
  const ReleaseGate release{.gate = backend->gate, .adapter = &adapter};
  RequestExecution first;
  RequestExecution queued;
  RequestExecution rejected;
  auto running = Start(loop, adapter.GetTask(Request(), first.Context()));
  loop.poll();
  ASSERT_TRUE(backend->gate->WaitForEntry());
  auto waiting = Start(loop, adapter.GetTask(Request(), queued.Context()));
  auto full = Start(loop, adapter.GetTask(Request(), rejected.Context()));
  loop.poll();
  ASSERT_EQ(full.wait_for(kNoWait), std::future_status::ready);
  EXPECT_FALSE(full.get().ok());
  adapter.Shutdown();
  auto closed = Start(loop, adapter.GetTask(Request(), rejected.Context()));
  loop.poll();
  ASSERT_EQ(closed.wait_for(kNoWait), std::future_status::ready);
  EXPECT_FALSE(closed.get().ok());
  EXPECT_EQ(running.wait_for(kNoWait), std::future_status::timeout);
  ASSERT_TRUE(queued.Cancel());
  backend->gate->Open();
  loop.run();
  EXPECT_TRUE(running.get().ok());
  EXPECT_FALSE(waiting.get().ok());
  adapter.Join();
  adapter.Join();
  EXPECT_EQ(backend->calls.load(), kOne);
}

TEST(BlockingExecutorAdapterTest, PreCancelledRequestNeverReachesBackend) {
  auto backend = std::make_shared<Backend>();
  BlockingExecutorAdapter adapter(backend);
  asio::io_context loop;
  RequestExecution source;
  ASSERT_TRUE(source.Cancel());
  EXPECT_FALSE(AwaitResult(loop, adapter.GetTask(Request(), source.Context())).ok());
  EXPECT_EQ(backend->calls.load(), 0);
}

TEST(BlockingExecutorAdapterTest, RejectsJoinFromBackendInsteadOfDeadlocking) {
  auto backend = std::make_shared<Backend>();
  BlockingExecutorAdapter adapter(backend);
  backend->on_read = [&adapter] { adapter.Join(); };
  RequestExecution source;
  asio::io_context loop;
  const auto result = AwaitResult(loop, adapter.GetTask(Request(), source.Context()));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kInternal);
}

TEST(BlockingExecutorAdapterTest, ElapsedDeadlineNeverReachesBackend) {
  auto backend = std::make_shared<Backend>();
  BlockingExecutorAdapter adapter(backend);
  asio::io_context loop;
  constexpr auto kPast = std::chrono::milliseconds(1);
  RequestExecution source({}, RequestExecution::Clock::now() - kPast);
  EXPECT_FALSE(AwaitResult(loop, adapter.GetTask(Request(), source.Context())).ok());
  EXPECT_EQ(backend->calls.load(), 0);
}

bool RejectsWorkerJoin(AsyncRuntime& runtime) {
  try {
    runtime.Join();
  } catch (const std::logic_error&) {
    return true;
  }
  return false;
}

TEST(AsyncRuntimeTest, ShutdownFromCompletionDoesNotBlockOrReplaceSuccess) {
  auto backend = std::make_shared<test::CoroutineBackend>();
  AsyncRuntime runtime(backend);
  auto reply = std::make_shared<std::promise<RequestOutcome>>();
  auto completed = reply->get_future();
  const auto accepted =
      runtime.dispatcher()->Dispatch({.operation = DispatcherOperation::kGetTask, .payload = Request()}, {},
                                     [&runtime, reply](auto result, auto context) {
                                       const bool valid = result.ok() && RejectsWorkerJoin(runtime);
                                       runtime.Shutdown();
                                       reply->set_value(valid ? context.Outcome() : RequestOutcome::kFailed);
                                     });
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(completed.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(completed.get(), RequestOutcome::kSucceeded);
  runtime.Join();
}

TEST(AsyncRuntimeTest, RejectsInvalidConfiguration) {
  auto backend = std::make_shared<Backend>();
  EXPECT_THROW(BlockingExecutorAdapter(nullptr), std::invalid_argument);
  EXPECT_THROW((BlockingExecutorAdapter(backend, {.workers = 0})), std::invalid_argument);
  EXPECT_THROW((BlockingExecutorAdapter(backend, {.max_pending = 0})), std::invalid_argument);
  auto executor = std::make_shared<test::CoroutineBackend>();
  EXPECT_THROW((AsyncRuntime(executor, {}, {.threads = 0})), std::invalid_argument);
  EXPECT_THROW(AsyncRuntime(nullptr), std::invalid_argument);
}

TEST(AsyncRuntimeTest, ShutdownCompletesLogicallyBeforeBackendCleanupAndJoinDrains) {
  auto backend = std::make_shared<Backend>(Behavior::kWait);
  auto adapter = std::make_shared<BlockingExecutorAdapter>(backend);
  AsyncRuntime runtime(adapter);
  const ReleaseGate release{.gate = backend->gate};
  std::promise<RequestOutcome> reply;
  auto completed = reply.get_future();
  const auto accepted =
      runtime.dispatcher()->Dispatch({.operation = DispatcherOperation::kGetTask, .payload = Request()}, {},
                                     [&reply](auto, auto context) { reply.set_value(context.Outcome()); });
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(backend->gate->WaitForEntry());
  runtime.Shutdown();
  ASSERT_EQ(completed.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(completed.get(), RequestOutcome::kServerShutdown);
  backend->gate->Open();
  runtime.Join();
  runtime.Join();
  EXPECT_EQ(backend->calls.load(), kOne);
  EXPECT_FALSE(runtime.dispatcher()
                   ->Dispatch({.operation = DispatcherOperation::kGetTask, .payload = Request()}, {}, [](auto, auto) {})
                   .ok());
}

TEST(AsyncRuntimeTest, DestructionCancelsAndDrainsNativeSuspendingCoroutine) {
  const auto backend = std::make_shared<test::CoroutineBackend>(test::CoroutineBehavior::kWait);
  std::promise<RequestOutcome> reply;
  auto completed = reply.get_future();
  {
    AsyncRuntime runtime(backend);
    const auto accepted =
        runtime.dispatcher()->Dispatch({.operation = DispatcherOperation::kGetTask, .payload = Request()}, {},
                                       [&reply](auto, auto context) { reply.set_value(context.Outcome()); });
    ASSERT_TRUE(accepted.ok());
    ASSERT_TRUE(backend->WaitStarted(kOne));
  }
  ASSERT_EQ(completed.wait_for(kNoWait), std::future_status::ready);
  EXPECT_EQ(completed.get(), RequestOutcome::kServerShutdown);
  EXPECT_EQ(backend->completed.load(), kOne);
}
}  // namespace
