// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <boost/asio/executor_work_guard.hpp>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "support/asio_test_support.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
using namespace a2a::test;
constexpr std::size_t kOnePending = 1;
constexpr auto kRaceParticipants = 2;

struct Reply final {
  std::promise<core::Result<DispatchResponse>> promise;
  std::future<core::Result<DispatchResponse>> result = promise.get_future();
  std::atomic_size_t count = 0;
  std::atomic<RequestOutcome> outcome = RequestOutcome::kPending;
};

class CoroutineDispatcherTest : public testing::Test {
 protected:
  void SetUp() override { Configure(); }
  void TearDown() override {
    dispatcher.reset();
    if (application != nullptr) {
      application->Release();
    }
    guard.reset();
    for (auto& worker : workers) {
      worker.join();
    }
    loop.restart();
    loop.run();
  }
  void StartWorkers() {
    guard.emplace(asio::make_work_guard(loop));
    workers.reserve(kCoroutineWorkers);
    for (std::size_t index = 0; index < kCoroutineWorkers; ++index) {
      workers.emplace_back([this] { loop.run(); });
    }
  }
  void Configure(CoroutineBehavior behavior = CoroutineBehavior::kImmediate,
                 std::size_t capacity = AsyncDispatcherOptions::kDefaultMaxInFlight) {
    application = std::make_shared<CoroutineBackend>(behavior);
    AsyncDispatcherOptions options;
    options.max_pending = capacity;
    dispatcher = std::make_unique<AsyncDispatcher>(application, loop.get_executor(), options);
  }
  std::shared_ptr<RequestExecution> Send(const std::shared_ptr<Reply>& reply,
                                         DispatchRequest request = CoroutineRequest(), RequestContext metadata = {}) {
    auto accepted = dispatcher->Dispatch(std::move(request), std::move(metadata),
                                         [reply](auto response, RequestExecutionContext context) {
                                           reply->outcome = context.Outcome();
                                           if (reply->count.fetch_add(1) == 0) {
                                             reply->promise.set_value(std::move(response));
                                           }
                                         });
    EXPECT_TRUE(accepted.ok());
    return accepted.ok() ? accepted.value() : nullptr;
  }
  core::Result<DispatchResponse> Receive(const std::shared_ptr<Reply>& reply) {
    const auto ready = workers.empty() ? PumpReady(loop, reply->result)
                                       : reply->result.wait_for(kCoroutineWait) == std::future_status::ready;
    if (!ready) {
      ADD_FAILURE() << kCoroutineFailure;
      return core::Error::Internal(kCoroutineFailure);
    }
    return reply->result.get();
  }
  bool Started(std::size_t count = 1) {
    return workers.empty() ? PumpUntil(loop, [&] { return application->started >= count; })
                           : application->WaitStarted(count);
  }
  bool Finished(std::size_t count = 1) {
    return PumpUntil(loop, [&] { return application->completed >= count; });
  }
  void ExpectAdmissionAfterCleanup() {
    application->Release();
    ASSERT_TRUE(Finished());
    loop.restart();
    loop.run();
    const auto next = std::make_shared<Reply>();
    ASSERT_NE(Send(next), nullptr);
    EXPECT_TRUE(Receive(next).ok());
  }
  asio::io_context loop;
  std::optional<asio::executor_work_guard<asio::io_context::executor_type>> guard;
  std::vector<std::thread> workers;
  std::shared_ptr<CoroutineBackend> application;
  std::unique_ptr<AsyncDispatcher> dispatcher;
};

TEST_F(CoroutineDispatcherTest, ReturnsOwnedRequestAndMetadataThroughDispatcher) {
  const auto reply = std::make_shared<Reply>();
  RequestContext metadata;
  metadata.request_id = kCoroutineRequestId;
  EXPECT_NE(Send(reply, CoroutineRequest(), std::move(metadata)), nullptr);
  const auto result = Receive(reply);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(std::get<lf::a2a::v1::Task>(result.value().payload()).id(), kCoroutineTaskId);
  EXPECT_EQ(application->Metadata().request_id, kCoroutineRequestId);
  EXPECT_EQ(reply->outcome, RequestOutcome::kSucceeded);
}

TEST_F(CoroutineDispatcherTest, RoutesEveryUnaryOperationIncludingVoid) {
  for (auto& request : CoroutineUnaryRequests()) {
    const auto reply = std::make_shared<Reply>();
    ASSERT_NE(Send(reply, std::move(request)), nullptr);
    EXPECT_TRUE(Receive(reply).ok());
    EXPECT_EQ(reply->count, 1);
  }
}

TEST_F(CoroutineDispatcherTest, UnimplementedOperationsReturnUnsupported) {
  dispatcher = std::make_unique<AsyncDispatcher>(std::make_shared<CoroutineAgentExecutor>(), loop.get_executor());
  for (auto& request : CoroutineUnaryRequests()) {
    const auto reply = std::make_shared<Reply>();
    ASSERT_NE(Send(reply, std::move(request)), nullptr);
    EXPECT_FALSE(Receive(reply).ok());
    EXPECT_EQ(reply->outcome, RequestOutcome::kFailed);
  }
}

TEST_F(CoroutineDispatcherTest, PreservesApplicationErrors) {
  Configure(CoroutineBehavior::kError);
  const auto reply = std::make_shared<Reply>();
  ASSERT_NE(Send(reply), nullptr);
  const auto result = Receive(reply);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kValidation);
}

TEST_F(CoroutineDispatcherTest, ConvertsUnhandledExceptionsToInternalErrors) {
  Configure(CoroutineBehavior::kThrow);
  const auto reply = std::make_shared<Reply>();
  ASSERT_NE(Send(reply), nullptr);
  const auto result = Receive(reply);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), core::ErrorCode::kInternal);
}

TEST_F(CoroutineDispatcherTest, CancellationAbortsSuspendedTimerAndCompletesOnce) {
  Configure(CoroutineBehavior::kWait);
  const auto reply = std::make_shared<Reply>();
  const auto source = Send(reply);
  ASSERT_NE(source, nullptr);
  ASSERT_TRUE(Started());
  dispatcher->Cancel(source);
  EXPECT_FALSE(Receive(reply).ok());
  EXPECT_EQ(reply->outcome, RequestOutcome::kClientCancelled);
  EXPECT_TRUE(Finished());
  loop.restart();
  loop.poll();
  EXPECT_EQ(reply->count, 1);
}

TEST_F(CoroutineDispatcherTest, DeadlineSignalReachesSuspendedOperation) {
  Configure(CoroutineBehavior::kWait);
  const auto reply = std::make_shared<Reply>();
  const auto source = Send(reply);
  ASSERT_NE(source, nullptr);
  ASSERT_TRUE(Started());
  dispatcher->Cancel(source, RequestCancellationReason::kDeadlineExceeded);
  EXPECT_FALSE(Receive(reply).ok());
  EXPECT_EQ(reply->outcome, RequestOutcome::kDeadlineExceeded);
  EXPECT_TRUE(Finished());
}

TEST_F(CoroutineDispatcherTest, ShutdownSchedulesCancellationWithoutRunningLoopInline) {
  Configure(CoroutineBehavior::kWait);
  const auto reply = std::make_shared<Reply>();
  ASSERT_NE(Send(reply), nullptr);
  ASSERT_TRUE(Started());
  dispatcher->Shutdown();
  EXPECT_EQ(application->completed, 0);
  EXPECT_FALSE(Receive(reply).ok());
  EXPECT_EQ(reply->outcome, RequestOutcome::kServerShutdown);
  EXPECT_TRUE(Finished());
}

TEST_F(CoroutineDispatcherTest, CancellationBeforeEventLoopStartsSkipsApplication) {
  const auto reply = std::make_shared<Reply>();
  const auto source = Send(reply);
  ASSERT_NE(source, nullptr);
  EXPECT_EQ(application->started, 0);
  EXPECT_TRUE(source->Cancel());
  EXPECT_FALSE(Receive(reply).ok());
  EXPECT_EQ(application->started, 0);
}

TEST_F(CoroutineDispatcherTest, SuspendedCoroutinesDoNotOccupyWorkers) {
  Configure(CoroutineBehavior::kWait);
  std::vector<std::shared_ptr<Reply>> replies;
  replies.reserve(kCoroutineRequests);
  for (std::size_t index = 0; index < kCoroutineRequests; ++index) {
    const auto reply = std::make_shared<Reply>();
    ASSERT_NE(Send(reply), nullptr);
    replies.push_back(reply);
  }
  ASSERT_TRUE(Started(kCoroutineRequests));
  EXPECT_EQ(application->completed, 0);
  application->Release();
  for (const auto& reply : replies) {
    EXPECT_TRUE(Receive(reply).ok());
  }
}

TEST_F(CoroutineDispatcherTest, CapacityIncludesCancelledCoroutinesUntilCleanup) {
  Configure(CoroutineBehavior::kIgnoreCancellation, kOnePending);
  const auto first = std::make_shared<Reply>();
  const auto source = Send(first);
  ASSERT_NE(source, nullptr);
  ASSERT_TRUE(Started());
  dispatcher->Cancel(source);
  EXPECT_FALSE(Receive(first).ok());
  const auto rejected = dispatcher->Dispatch(CoroutineRequest(), {}, [](auto, auto) {});
  EXPECT_FALSE(rejected.ok());
  EXPECT_EQ(application->started, 1);
  ExpectAdmissionAfterCleanup();
}

TEST_F(CoroutineDispatcherTest, RetainsExecutorAfterDispatcherDestructionUntilCoroutineFinishes) {
  Configure(CoroutineBehavior::kIgnoreCancellation);
  const auto reply = std::make_shared<Reply>();
  ASSERT_NE(Send(reply), nullptr);
  ASSERT_TRUE(Started());
  const std::weak_ptr<CoroutineBackend> weak = application;
  dispatcher.reset();
  application.reset();
  EXPECT_FALSE(Receive(reply).ok());
  ASSERT_FALSE(weak.expired());
  weak.lock()->Release();
  EXPECT_TRUE(PumpUntil(loop, [&] { return weak.expired(); }));
  EXPECT_EQ(reply->count, 1);
}

TEST_F(CoroutineDispatcherTest, ThrowingCompletionDoesNotEscapeEventLoop) {
  const auto accepted =
      dispatcher->Dispatch(CoroutineRequest(), {}, [](auto, auto) { throw std::runtime_error(kCoroutineFailure); });
  EXPECT_TRUE(accepted.ok());
  EXPECT_NO_THROW(loop.run());
  EXPECT_EQ(application->completed, 1);
}

TEST_F(CoroutineDispatcherTest, ConcurrentCancellationAndCompletionOnMultipleIoWorkers) {
  Configure(CoroutineBehavior::kWait);
  StartWorkers();
  std::vector<std::shared_ptr<Reply>> replies;
  std::vector<std::shared_ptr<RequestExecution>> sources;
  replies.reserve(kCoroutineRequests);
  sources.reserve(kCoroutineRequests);
  for (std::size_t index = 0; index < kCoroutineRequests; ++index) {
    const auto reply = std::make_shared<Reply>();
    sources.push_back(Send(reply));
    replies.push_back(reply);
  }
  ASSERT_TRUE(Started(kCoroutineRequests));
  std::barrier start(kRaceParticipants);
  std::jthread cancel([&] {
    start.arrive_and_wait();
    for (const auto& source : sources) {
      dispatcher->Cancel(source);
    }
  });
  start.arrive_and_wait();
  application->Release();
  cancel.join();
  for (const auto& reply : replies) {
    const auto result = Receive(reply);
    EXPECT_EQ(reply->count, 1);
    EXPECT_TRUE(result.ok() || reply->outcome == RequestOutcome::kClientCancelled);
  }
  EXPECT_TRUE(application->WaitFinished(kCoroutineRequests));
}

TEST(CoroutineDispatcherOptions, RejectsMissingApplicationSchedulerAndCapacity) {
  asio::io_context loop;
  const auto application = std::make_shared<CoroutineBackend>();
  EXPECT_THROW(AsyncDispatcher(nullptr, loop.get_executor()), std::invalid_argument);
  EXPECT_THROW(AsyncDispatcher(application, {}), std::invalid_argument);
  AsyncDispatcherOptions invalid;
  invalid.max_pending = 0;
  EXPECT_THROW(AsyncDispatcher(application, loop.get_executor(), invalid), std::invalid_argument);
}
}  // namespace
