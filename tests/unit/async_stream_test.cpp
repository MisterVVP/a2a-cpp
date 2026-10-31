// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <barrier>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <future>
#include <memory>
#include <utility>

#include "a2a/server/engine/async_dispatcher.h"
#include "a2a/server/engine/async_runtime.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "support/async_stream_test_support.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
using namespace a2a::test;
class StreamAfter final : public ServerInterceptor {
 public:
  void AfterDispatch(const DispatchRequest& /*request*/, RequestContext& /*context*/,
                     const core::Result<DispatchResponse>& result) override {
    ++count;
    success = result.ok();
  }
  std::size_t count = 0;
  bool success = false;
};
class StreamSecurity final : public SecurityPolicy {
 public:
  [[nodiscard]] core::Result<std::string> Authenticate(const RequestContext& /*context*/) const override {
    return std::string(kStreamTenant);
  }
  [[nodiscard]] bool AuthorizeOperation(const DispatchRequest& /*request*/,
                                        const RequestContext& /*context*/) const override {
    return true;
  }
  [[nodiscard]] bool CanAccessTask(const lf::a2a::v1::Task& task, const RequestContext& context) const override {
    return task.context_id() == context.principal;
  }
};
class AsyncStreamTest : public testing::Test {
 protected:
  AsyncStreamTest() {
    AsyncDispatcherOptions options;
    options.interceptors = {after};
    dispatcher = std::make_unique<AsyncDispatcher>(application, loop.get_executor(), options);
  }
  void TearDown() override {
    if (dispatcher != nullptr) {
      dispatcher->Shutdown();
    }
    loop.restart();
    loop.run();
  }
  std::shared_ptr<RequestExecution> Send(bool subscribe = false,
                                         std::optional<RequestExecution::Clock::time_point> deadline = {}) {
    auto accepted = dispatcher->DispatchStream(
        StreamRequest(subscribe), {}, sink,
        [this](auto response, auto context) {
          ++deliveries;
          outcome = context.Outcome();
          completion.set_value(response.ok());
        },
        deadline);
    EXPECT_TRUE(accepted.ok());
    return accepted.ok() ? accepted.value() : nullptr;
  }
  bool Receive() {
    const bool ready = PumpReady(loop, result);
    EXPECT_TRUE(ready);
    return ready && result.get();
  }
  bool Events(std::size_t count) {
    return PumpUntil(loop, [&] { return sink->events.size() >= count; });
  }
  boost::asio::io_context loop;
  InMemoryTaskStore protected_store;
  std::shared_ptr<StreamBackend> application = std::make_shared<StreamBackend>();
  std::shared_ptr<FakeStreamSink> sink = std::make_shared<FakeStreamSink>();
  std::shared_ptr<StreamAfter> after = std::make_shared<StreamAfter>();
  std::unique_ptr<AsyncDispatcher> dispatcher;
  std::promise<bool> completion;
  std::future<bool> result = completion.get_future();
  std::size_t deliveries = 0;
  RequestOutcome outcome = RequestOutcome::kPending;
};
TEST_F(AsyncStreamTest, FiniteProducerOwnsEventsAndPreservesOrder) {
  ASSERT_NE(Send(), nullptr);
  EXPECT_TRUE(Receive());
  ASSERT_EQ(sink->events.size(), kStreamEvents);
  for (std::size_t index = 0; index < kStreamEvents; ++index) {
    EXPECT_EQ(sink->events[index].message().message_id(), std::to_string(index));
  }
  EXPECT_EQ(outcome, RequestOutcome::kSucceeded);
  EXPECT_EQ(after->count, 1);
}
TEST_F(AsyncStreamTest, DemandAcknowledgementPrecedesNextEventAndAfter) {
  sink->automatic = false;
  ASSERT_NE(Send(), nullptr);
  ASSERT_TRUE(Events(1));
  EXPECT_EQ(application->acknowledged, 0);
  EXPECT_EQ(after->count, 0);
  for (std::size_t index = 0; index < kStreamEvents; ++index) {
    ASSERT_TRUE(Events(index + 1));
    EXPECT_EQ(sink->events.size(), index + 1);
    sink->Acknowledge();
  }
  EXPECT_TRUE(Receive());
  EXPECT_EQ(application->acknowledged, kStreamEvents);
  EXPECT_EQ(after->count, 1);
}
TEST_F(AsyncStreamTest, WriteFailurePropagatesAndStopsProduction) {
  sink->automatic = false;
  ASSERT_NE(Send(), nullptr);
  ASSERT_TRUE(Events(1));
  sink->Acknowledge(false);
  EXPECT_FALSE(Receive());
  EXPECT_EQ(sink->events.size(), 1);
  EXPECT_EQ(outcome, RequestOutcome::kFailed);
  EXPECT_EQ(after->count, 1);
}
TEST_F(AsyncStreamTest, BackendErrorAfterEventsFinishesExactlyOnce) {
  application->fail = true;
  ASSERT_NE(Send(), nullptr);
  EXPECT_FALSE(Receive());
  EXPECT_EQ(sink->events.size(), kStreamEvents);
  EXPECT_EQ(deliveries, 1);
  EXPECT_EQ(after->count, 1);
}
TEST_F(AsyncStreamTest, CancelWakesSuspendedDemandAndIgnoresLateAcknowledgement) {
  sink->automatic = false;
  const auto source = Send();
  ASSERT_TRUE(Events(1));
  dispatcher->Cancel(source);
  dispatcher->Cancel(source);
  EXPECT_FALSE(Receive());
  sink->Acknowledge();
  loop.restart();
  loop.poll();
  EXPECT_EQ(outcome, RequestOutcome::kClientCancelled);
  EXPECT_EQ(deliveries, 1);
  EXPECT_EQ(after->count, 1);
}
TEST_F(AsyncStreamTest, ExpiredDeadlineNeverStartsProducer) {
  ASSERT_NE(Send(false, RequestExecution::Clock::now()), nullptr);
  EXPECT_FALSE(Receive());
  EXPECT_EQ(outcome, RequestOutcome::kDeadlineExceeded);
  EXPECT_EQ(application->started, 0);
}
TEST_F(AsyncStreamTest, DeadlineDuringWriteWinsOverLateSuccess) {
  sink->automatic = false;
  const auto source = Send();
  ASSERT_TRUE(Events(1));
  dispatcher->Cancel(source, RequestCancellationReason::kDeadlineExceeded);
  EXPECT_FALSE(Receive());
  EXPECT_EQ(outcome, RequestOutcome::kDeadlineExceeded);
  EXPECT_EQ(after->count, 1);
}
TEST_F(AsyncStreamTest, ShutdownClosesAdmissionAndCancelsSuspendedWrite) {
  sink->automatic = false;
  ASSERT_NE(Send(), nullptr);
  ASSERT_TRUE(Events(1));
  dispatcher->Shutdown();
  EXPECT_FALSE(Receive());
  EXPECT_EQ(outcome, RequestOutcome::kServerShutdown);
  EXPECT_FALSE(dispatcher->DispatchStream(StreamRequest(), {}, sink, [](auto, auto) {}).ok());
}
lf::a2a::v1::StreamResponse ReadSynchronousEvent(ServerStreamSession& stream) {
  auto result = stream.Next();
  EXPECT_TRUE(result.ok());
  if (!result.ok()) {
    return {};
  }
  const auto& event = result.value();
  EXPECT_TRUE(event.has_value());
  return event.value_or(lf::a2a::v1::StreamResponse{});
}
TEST_F(AsyncStreamTest, MixedSyncAndAsyncSubscriptionsShareOrderedPublication) {
  auto synchronous = application->publications.Subscribe(StreamTask());
  ASSERT_TRUE(synchronous.ok());
  EXPECT_TRUE(ReadSynchronousEvent(*synchronous.value()).has_task());
  ASSERT_NE(Send(true), nullptr);
  ASSERT_TRUE(Events(1));
  auto completed = StreamTask(lf::a2a::v1::TASK_STATE_COMPLETED);
  application->publications.PublishTaskUpdated(completed);
  EXPECT_TRUE(Receive());
  ASSERT_EQ(sink->events.size(), 2);
  const auto sync_terminal = ReadSynchronousEvent(*synchronous.value());
  EXPECT_EQ(sync_terminal.SerializeAsString(), sink->events.back().SerializeAsString());
  EXPECT_FALSE(synchronous.value()->Next().value().has_value());
}
TEST_F(AsyncStreamTest, SlowSubscriberOverflowFailsWithoutHoldingPublisher) {
  sink->automatic = false;
  ASSERT_NE(Send(true), nullptr);
  ASSERT_TRUE(Events(1));
  for (std::size_t index = 0; index <= kStreamSmallCapacity; ++index) {
    application->publications.PublishTaskUpdated(StreamTask());
  }
  sink->Acknowledge();
  EXPECT_FALSE(Receive());
  EXPECT_EQ(after->count, 1);
  EXPECT_EQ(sink->events.size(), 1);
}
TEST_F(AsyncStreamTest, PublicationShutdownWakesIdleSubscription) {
  ASSERT_NE(Send(true), nullptr);
  ASSERT_TRUE(Events(1));
  application->publications.Shutdown();
  EXPECT_FALSE(Receive());
  EXPECT_EQ(after->count, 1);
}
TEST_F(AsyncStreamTest, CancellationWakesIdlePublicationWait) {
  const auto source = Send(true);
  ASSERT_TRUE(Events(1));
  dispatcher->Cancel(source);
  EXPECT_FALSE(Receive());
  EXPECT_EQ(outcome, RequestOutcome::kClientCancelled);
}
TEST_F(AsyncStreamTest, MismatchedPayloadSkipsApplication) {
  auto request = StreamRequest(true);
  request.payload = ListTasksRequest{};
  EXPECT_TRUE(
      dispatcher->DispatchStream(request, {}, sink, [this](auto value, auto) { completion.set_value(value.ok()); })
          .ok());
  EXPECT_FALSE(Receive());
  EXPECT_EQ(application->started, 0);
}
TEST_F(AsyncStreamTest, RechecksTaskAuthorizationBeforeEachEvent) {
  auto& store = protected_store;
  ASSERT_TRUE(store.CreateOrUpdate(StreamTask()).ok());
  AsyncDispatcherOptions options;
  options.security_policy = std::make_shared<StreamSecurity>();
  options.task_store = &store;
  options.interceptors = {after};
  dispatcher = std::make_unique<AsyncDispatcher>(application, loop.get_executor(), options);
  sink->automatic = false;
  ASSERT_NE(Send(), nullptr);
  ASSERT_TRUE(Events(1));
  auto task = StreamTask();
  task.clear_context_id();
  ASSERT_TRUE(store.CreateOrUpdate(task).ok());
  sink->Acknowledge();
  EXPECT_FALSE(Receive());
  EXPECT_EQ(sink->events.size(), 1);
  EXPECT_EQ(after->count, 1);
  dispatcher.reset();
  loop.restart();
  loop.run();
}
class IgnoringStreamBackend final : public CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<core::Result<void>> SendStreamingMessage(
      lf::a2a::v1::SendMessageRequest /*request*/, RequestExecutionContext /*context*/,
      std::shared_ptr<AsyncServerStreamSession> stream) override {
    retained = stream;
    lf::a2a::v1::StreamResponse event;
    event.mutable_message()->set_task_id(kStreamTask);
    if (concurrent) {
      using namespace boost::asio::experimental::awaitable_operators;
      (void)co_await (stream->Write(event) && stream->Write(event));
    } else {
      (void)co_await stream->Write(event);
    }
    co_return core::Result<void>{};
  }
  bool concurrent = false;
  std::shared_ptr<AsyncServerStreamSession> retained;
};
TEST_F(AsyncStreamTest, ConcurrentWritesFailRequestWithoutOverlappingTransportWrites) {
  auto invalid = std::make_shared<IgnoringStreamBackend>();
  invalid->concurrent = true;
  dispatcher = std::make_unique<AsyncDispatcher>(invalid, loop.get_executor());
  sink->automatic = false;
  ASSERT_NE(Send(), nullptr);
  ASSERT_TRUE(Events(1));
  sink->Acknowledge();
  EXPECT_FALSE(Receive());
  EXPECT_EQ(sink->events.size(), 1);
}
TEST_F(AsyncStreamTest, IgnoredTransportFailureStillDeterminesTerminalResult) {
  auto invalid = std::make_shared<IgnoringStreamBackend>();
  dispatcher = std::make_unique<AsyncDispatcher>(invalid, loop.get_executor());
  sink->automatic = false;
  ASSERT_NE(Send(), nullptr);
  ASSERT_TRUE(Events(1));
  sink->Acknowledge(false);
  EXPECT_FALSE(Receive());
  EXPECT_EQ(outcome, RequestOutcome::kFailed);
}
class ConcurrentStreamSink final : public AsyncServerStreamSession {
 public:
  boost::asio::awaitable<core::Result<void>> Write(lf::a2a::v1::StreamResponse /*event*/) override {
    {
      std::lock_guard lock(mutex_);
      ++events_;
    }
    changed_.notify_all();
    co_return core::Result<void>{};
  }
  bool WaitInitial() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kCoroutineWait, [this] { return events_ != 0; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t events_ = 0;
};
void RaceTerminalPublicationAndCancellation() {
  constexpr auto kParticipants = 2;
  auto application = std::make_shared<StreamBackend>();
  auto sink = std::make_shared<ConcurrentStreamSink>();
  AsyncRuntime runtime(application);
  auto completion = std::make_shared<std::promise<RequestOutcome>>();
  auto result = completion->get_future();
  auto deliveries = std::make_shared<std::atomic_size_t>(0);
  auto accepted = runtime.dispatcher()->DispatchStream(
      StreamRequest(true), {}, sink, [completion, deliveries](auto, const RequestExecutionContext& context) {
        ++*deliveries;
        completion->set_value(context.Outcome());
      });
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(sink->WaitInitial());
  std::barrier race(kParticipants);
  std::jthread publisher([&] {
    race.arrive_and_wait();
    application->publications.PublishTaskUpdated(StreamTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  });
  std::jthread cancellation([&] {
    race.arrive_and_wait();
    runtime.dispatcher()->Cancel(accepted.value());
  });
  publisher.join();
  cancellation.join();
  ASSERT_EQ(result.wait_for(kCoroutineWait), std::future_status::ready);
  const auto outcome = result.get();
  EXPECT_TRUE(outcome == RequestOutcome::kSucceeded || outcome == RequestOutcome::kClientCancelled);
  runtime.Join();
  EXPECT_EQ(deliveries->load(), 1);
}
TEST(AsyncStreamConcurrency, PublicationAndCancellationOnIndependentThreadsCompleteOnce) {
  constexpr std::size_t kRaceIterations = 16;
  for (std::size_t iteration = 0; iteration < kRaceIterations; ++iteration) {
    RaceTerminalPublicationAndCancellation();
  }
}
}  // namespace
