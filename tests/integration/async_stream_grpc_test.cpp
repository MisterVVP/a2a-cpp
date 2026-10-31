// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "a2a/client/client.h"
#include "a2a/client/grpc_transport.h"
#include "support/async_stream_grpc_support.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
using namespace a2a::test;
constexpr auto kShortStreamDeadline = std::chrono::milliseconds(500);
constexpr auto kStreamRaceParticipants = 2;
class NativeStreamAfter final : public ServerInterceptor {
 public:
  void AfterDispatch(const DispatchRequest& /*request*/, RequestContext& /*context*/,
                     const core::Result<DispatchResponse>& /*result*/) override {
    ++count;
  }
  std::atomic_size_t count = 0;
};
TEST(AsyncStreamGrpc, SendsOrderedFiniteEventsAndExtensionTrailers) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  const auto call = server.Start();
  for (std::size_t index = 0; index < kStreamEvents; ++index) {
    ASSERT_TRUE(call->Read());
    EXPECT_EQ(call->event.message().message_id(), std::to_string(index));
  }
  EXPECT_FALSE(call->Read());
  EXPECT_TRUE(call->Finish().ok());
  const auto& trailers = call->context.GetServerTrailingMetadata();
  EXPECT_EQ(trailers.count(kStreamExtensionKey), 1);
}
TEST(AsyncStreamGrpc, SubscriptionConvertsTenantAndPropagatesOwnedMetadata) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  const auto call = server.Start(true);
  ASSERT_TRUE(call->Read());
  EXPECT_TRUE(call->event.has_task());
  {
    std::lock_guard lock(server.application->mutex);
    EXPECT_EQ(server.application->tenant, kStreamTenant);
    EXPECT_EQ(server.application->metadata.client_headers.at(kStreamHeader), kStreamMetadata);
  }
  server.application->publications.PublishTaskUpdated(StreamTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  ASSERT_TRUE(call->Read());
  EXPECT_EQ(call->event.status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  EXPECT_FALSE(call->Read());
  EXPECT_TRUE(call->Finish().ok());
}
TEST(AsyncStreamGrpc, BackendErrorAfterEventsPreservesErrorStatus) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  server.application->fail = true;
  const auto call = server.Start();
  while (call->Read()) {
  }
  EXPECT_EQ(call->Finish().error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
}
TEST(AsyncStreamGrpc, RequiredMetadataIsValidatedBeforeApplicationExecution) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  const auto call = server.Start(false, false);
  EXPECT_FALSE(call->Read());
  EXPECT_FALSE(call->Finish().ok());
  EXPECT_EQ(server.application->started, 0);
}
TEST(AsyncStreamGrpc, ClientCancellationFinishesAndRunsAfterExactlyOnce) {
  auto after = std::make_shared<NativeStreamAfter>();
  {
    NativeStreamServer server({after});
    const auto call = server.Start(true);
    ASSERT_TRUE(call->Read());
    call->context.TryCancel();
    EXPECT_FALSE(call->Read());
    EXPECT_EQ(call->Finish().error_code(), ::grpc::StatusCode::CANCELLED);
  }
  EXPECT_EQ(after->count, 1);
}
TEST(AsyncStreamGrpc, NativeDeadlineCancelsIdleSubscription) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  const auto call = server.Start(true, true, kShortStreamDeadline);
  ASSERT_TRUE(call->Read());
  EXPECT_FALSE(call->Read());
  EXPECT_EQ(call->Finish().error_code(), ::grpc::StatusCode::DEADLINE_EXCEEDED);
}
TEST(AsyncStreamGrpc, ShutdownCompletesIdleStreamsWithUnavailable) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  const auto call = server.Start(true);
  ASSERT_TRUE(call->Read());
  server.Shutdown();
  EXPECT_FALSE(call->Read());
  EXPECT_EQ(call->Finish().error_code(), ::grpc::StatusCode::UNAVAILABLE);
  const auto rejected = server.Start(true);
  EXPECT_FALSE(rejected->Read());
  EXPECT_EQ(rejected->Finish().error_code(), ::grpc::StatusCode::UNAVAILABLE);
}
void ExpectNativeTerminal(const std::shared_ptr<NativeStream>& call) {
  EXPECT_TRUE(call->Read());
  EXPECT_EQ(call->event.status_update().status().state(), lf::a2a::v1::TASK_STATE_COMPLETED);
  EXPECT_FALSE(call->Read());
  EXPECT_TRUE(call->Finish().ok());
}
TEST(AsyncStreamGrpc, SixtyFourIdleStreamsShareFixedWorkersAndDeliverTerminalEvents) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  std::vector<std::shared_ptr<NativeStream>> calls;
  calls.reserve(kStreamConcurrency);
  for (std::size_t index = 0; index < kStreamConcurrency; ++index) {
    auto call = server.Start(true);
    ASSERT_TRUE(call->Read());
    calls.push_back(std::move(call));
  }
  EXPECT_LE(server.application->ThreadCount(), kCoroutineWorkers);
  server.application->publications.PublishTaskUpdated(StreamTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  for (const auto& call : calls) {
    ExpectNativeTerminal(call);
  }
}
TEST(AsyncStreamGrpc, CompletionAndCancellationRaceKeepsCleanupSafe) {
  auto after = std::make_shared<NativeStreamAfter>();
  {
    NativeStreamServer server({after});
    const auto call = server.Start(true);
    ASSERT_TRUE(call->Read());
    std::barrier race(kStreamRaceParticipants);
    std::jthread cancel([&] {
      race.arrive_and_wait();
      call->context.TryCancel();
    });
    race.arrive_and_wait();
    server.application->publications.PublishTaskUpdated(StreamTask(lf::a2a::v1::TASK_STATE_COMPLETED));
    cancel.join();
    while (call->Read()) {
    }
    const auto status = call->Finish();
    EXPECT_TRUE(status.ok() || status.error_code() == ::grpc::StatusCode::CANCELLED);
  }
  EXPECT_EQ(after->count, 1);
}
TEST(AsyncStreamGrpc, CancelsSlowReaderWhileLargeWriteIsOutstanding) {
  constexpr std::size_t kLargeEventBytes = 1024UL * 1024UL;
  auto after = std::make_shared<NativeStreamAfter>();
  {
    NativeStreamServer server({after});
    server.application->event_count = kStreamConcurrency;
    server.application->payload_bytes = kLargeEventBytes;
    const auto call = server.Start();
    ASSERT_TRUE(server.application->WaitStarted());
    ASSERT_TRUE(call->Read());
    call->context.TryCancel();
    while (call->Read()) {
    }
    EXPECT_EQ(call->Finish().error_code(), ::grpc::StatusCode::CANCELLED);
  }
  EXPECT_EQ(after->count, 1);
}
class SdkStreamObserver final : public client::StreamObserver {
 public:
  void OnEvent(const lf::a2a::v1::StreamResponse& /*event*/) override {
    {
      std::lock_guard lock(mutex);
      ++events;
    }
    changed.notify_all();
  }
  void OnError(const core::Error& /*error*/) override { Complete(false); }
  void OnCompleted() override { Complete(true); }
  bool WaitEvents(std::size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kCoroutineWait, [&] { return events >= count; });
  }
  bool Finished() { return completion.wait_for(kCoroutineWait) == std::future_status::ready && completion.get(); }
  std::size_t Count() {
    std::lock_guard lock(mutex);
    return events;
  }

 private:
  void Complete(bool ok) {
    if (!completed.exchange(true)) {
      result.set_value(ok);
    }
  }
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t events = 0;
  std::atomic_bool completed = false;
  std::promise<bool> result;
  std::future<bool> completion = result.get_future();
};
TEST(AsyncStreamGrpc, ExistingSdkClientConsumesBothCoroutineStreamOperations) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  client::A2AClient sdk(
      std::make_unique<client::GrpcTransport>(client::ResolvedInterface{.transport = client::PreferredTransport::kGrpc,
                                                                        .url = server.Endpoint(),
                                                                        .security_requirements = {},
                                                                        .security_schemes = {}},
                                              server.Channel()));
  client::CallOptions options;
  options.extensions = {kStreamExtension};
  SdkStreamObserver messages;
  auto send = sdk.SendStreamingMessage(lf::a2a::v1::SendMessageRequest{}, messages, options);
  ASSERT_TRUE(send.ok()) << send.error().message();
  ASSERT_TRUE(messages.Finished());
  EXPECT_EQ(messages.Count(), kStreamEvents);
  SdkStreamObserver updates;
  lf::a2a::v1::GetTaskRequest query;
  query.set_id(kStreamTask);
  auto subscribe = sdk.SubscribeTask(query, updates, options);
  ASSERT_TRUE(subscribe.ok()) << subscribe.error().message();
  ASSERT_TRUE(updates.WaitEvents(1));
  server.application->publications.PublishTaskUpdated(StreamTask(lf::a2a::v1::TASK_STATE_COMPLETED));
  EXPECT_TRUE(updates.Finished());
  EXPECT_EQ(updates.Count(), 2);
}
class ShutdownWriteExecutor final : public CoroutineAgentExecutor {
 public:
  boost::asio::awaitable<core::Result<void>> SendStreamingMessage(
      lf::a2a::v1::SendMessageRequest /*request*/, RequestExecutionContext context,
      std::shared_ptr<AsyncServerStreamSession> stream) override {
    execution = std::make_shared<RequestExecutionContext>(std::move(context));
    lf::a2a::v1::StreamResponse event;
    event.mutable_message()->add_parts()->set_text(std::string(kBlockedWriteBytes, 'x'));
    const auto scheduler = co_await boost::asio::this_coro::executor;
    // This runs after Write has initiated and yielded the request's strand.
    boost::asio::post(scheduler, [this] { pending.set_value(); });
    co_return co_await stream->Write(std::move(event));
  }
  std::promise<void> pending;
  std::shared_ptr<RequestExecutionContext> execution;

 private:
  static constexpr std::size_t kBlockedWriteBytes = 3UL * 1024UL * 1024UL;
};
TEST(AsyncStreamGrpc, ShutdownDrainsHostWithAnUnreadFlowControlledWrite) {
  auto application = std::make_shared<ShutdownWriteExecutor>();
  auto pending = application->pending.get_future();
  auto after = std::make_shared<NativeStreamAfter>();
  AsyncDispatcherOptions options;
  options.interceptors = {after};
  AsyncRuntime runtime(application, std::move(options));
  AsyncGrpcServerTransport service(runtime.dispatcher());
  ::grpc::ServerBuilder builder;
  int port = 0;
  builder.AddListeningPort(kStreamBind, ::grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  ASSERT_NE(server, nullptr);
  std::string endpoint(kStreamEndpoint);
  endpoint.append(std::to_string(port));
  ::grpc::ChannelArguments arguments;
  arguments.SetInt(GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES, 0);
  arguments.SetInt(GRPC_ARG_HTTP2_BDP_PROBE, 0);
  auto stub = lf::a2a::v1::A2AService::NewStub(
      ::grpc::CreateCustomChannel(endpoint, ::grpc::InsecureChannelCredentials(), arguments));
  ::grpc::ClientContext context;
  context.AddMetadata(std::string(GrpcServerTransport::kVersionMetadataKey),
                      std::string(core::Version::kProtocolVersion));
  context.set_deadline(std::chrono::system_clock::now() + kStreamClientTimeout);
  auto reader = stub->SendStreamingMessage(&context, lf::a2a::v1::SendMessageRequest{});
  const bool write_started = pending.wait_for(kCoroutineWait) == std::future_status::ready;
  service.Shutdown();
  auto stopped = std::async(std::launch::async, [&] {
    server->Shutdown();
    server->Wait();
  });
  const bool drained = stopped.wait_for(kCoroutineWait) == std::future_status::ready;
  context.TryCancel();
  (void)reader->Finish();
  stopped.get();
  runtime.Join();
  EXPECT_TRUE(write_started);
  EXPECT_TRUE(drained);
  const auto& execution = application->execution;
  ASSERT_NE(execution, nullptr);
  EXPECT_EQ(execution->Outcome(), RequestOutcome::kServerShutdown);
  EXPECT_EQ(after->count, 1);
}
}  // namespace
