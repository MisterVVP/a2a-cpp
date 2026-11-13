// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
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
constexpr auto kMinimumStreamLookaheadBytes = 5;
constexpr std::size_t kBurstStreamEvents = 128;
#if defined(__linux__)
constexpr auto kGrpcStreamTaskDirectory = "/proc/self/task";
#endif
constexpr auto kIdleThreadAllowance = kStreamConcurrency / 2;
class NativeStreamAfter final : public ServerInterceptor {
 public:
  void AfterDispatch(const DispatchRequest& /*request*/, RequestContext& /*context*/,
                     const core::Result<DispatchResponse>& /*result*/) override {
    ++count;
  }
  std::atomic_size_t count = 0;
};
void ExpectOrderedNativeEvents(NativeStream& call, std::size_t event_count = kStreamEvents) {
  for (std::size_t index = 0; index < event_count; ++index) {
    ASSERT_TRUE(call.Read());
    EXPECT_EQ(call.event.message().message_id(), std::to_string(index));
  }
}
void ExpectSubscriptionMetadata(StreamBackend& application) {
  std::lock_guard lock(application.mutex);
  EXPECT_EQ(application.tenant, kStreamTenant);
  EXPECT_EQ(application.metadata.client_headers.at(kStreamHeader), kStreamMetadata);
}
TEST(AsyncStreamGrpc, SendsOrderedFiniteEventsAndExtensionTrailers) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  const auto call = server.Start();
  ExpectOrderedNativeEvents(*call);
  EXPECT_FALSE(call->Read());
  EXPECT_TRUE(call->Finish().ok());
  const auto& trailers = call->context.GetServerTrailingMetadata();
  EXPECT_EQ(trailers.count(kStreamExtensionKey), 1);
}
TEST(AsyncStreamGrpc, RepeatedWritesAcknowledgeEachEventAndFinishInOrder) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  server.application->event_count = kBurstStreamEvents;
  const auto call = server.Start();
  ExpectOrderedNativeEvents(*call, kBurstStreamEvents);
  EXPECT_FALSE(call->Read());
  EXPECT_TRUE(call->Finish().ok());
  EXPECT_EQ(server.application->acknowledged, kBurstStreamEvents);
}
TEST(AsyncStreamGrpc, SubscriptionConvertsTenantAndPropagatesOwnedMetadata) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  const auto call = server.Start(true);
  ASSERT_TRUE(call->Read());
  EXPECT_TRUE(call->event.has_task());
  ExpectSubscriptionMetadata(*server.application);
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
  ASSERT_TRUE(server.application->WaitSubscriptionAcknowledged());
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
    // Poll the synchronous client's CQ so Windows can progress the RPC, without
    // consuming event data or releasing the stream's receive demand.
    call->reader->WaitForInitialMetadata();
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
  void OnEvent(const lf::a2a::v1::StreamResponse& event) override {
    {
      std::lock_guard lock(mutex);
      ++events;
      ids.push_back(event.message().message_id());
    }
    changed.notify_all();
    if (on_event) {
      on_event();
    }
  }
  void OnError(const core::Error& error) override {
    {
      std::lock_guard lock(mutex);
      protocol_code = error.protocol_code().value_or(std::string{});
    }
    ++terminals;
    Complete(false);
  }
  void OnCompleted() override {
    ++terminals;
    Complete(true);
  }
  bool WaitEvents(std::size_t count) {
    std::unique_lock lock(mutex);
    return changed.wait_for(lock, kCoroutineWait, [&] { return events >= count; });
  }
  bool Finished() { return completion.wait_for(kCoroutineWait) == std::future_status::ready && completion.get(); }
  std::size_t Count() {
    std::lock_guard lock(mutex);
    return events;
  }
  std::vector<std::string> Ids() {
    std::lock_guard lock(mutex);
    return ids;
  }
  std::string ProtocolCode() {
    std::lock_guard lock(mutex);
    return protocol_code;
  }
  std::function<void()> on_event;
  std::atomic_size_t terminals = 0;

 private:
  void Complete(bool ok) {
    if (!completed.exchange(true)) {
      result.set_value(ok);
    }
  }
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t events = 0;
  std::vector<std::string> ids;
  std::string protocol_code;
  std::atomic_bool completed = false;
  std::promise<bool> result;
  std::future<bool> completion = result.get_future();
};

void ExpectSdkMessageStream(client::A2AClient& sdk, const client::CallOptions& options) {
  SdkStreamObserver messages;
  auto send = sdk.SendStreamingMessage(lf::a2a::v1::SendMessageRequest{}, messages, options);
  ASSERT_TRUE(send.ok()) << send.error().message();
  ASSERT_TRUE(messages.Finished());
  EXPECT_EQ(messages.Count(), kStreamEvents);
}
std::unique_ptr<client::A2AClient> MakeStreamSdk(const NativeStreamServer& server) {
  return std::make_unique<client::A2AClient>(
      std::make_unique<client::GrpcTransport>(client::ResolvedInterface{.transport = client::PreferredTransport::kGrpc,
                                                                        .url = server.Endpoint(),
                                                                        .security_requirements = {},
                                                                        .security_schemes = {}},
                                              server.Channel()));
}
client::CallOptions SdkStreamOptions() {
  client::CallOptions options;
  options.extensions = {kStreamExtension};
  return options;
}
lf::a2a::v1::GetTaskRequest SdkSubscriptionRequest() {
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kStreamTask);
  return request;
}

TEST(AsyncStreamGrpc, SdkCallbackReaderPreservesBurstOrderAndRemoteErrors) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  server.application->event_count = kBurstStreamEvents;
  server.application->fail = true;
  auto sdk = MakeStreamSdk(server);
  SdkStreamObserver observer;
  auto stream = sdk->SendStreamingMessage({}, observer, SdkStreamOptions());
  ASSERT_TRUE(stream.ok());
  EXPECT_FALSE(observer.Finished());
  EXPECT_EQ(observer.terminals, 1);
  const auto ids = observer.Ids();
  ASSERT_EQ(ids.size(), kBurstStreamEvents);
  for (std::size_t index = 0; index < ids.size(); ++index) {
    EXPECT_EQ(ids[index], std::to_string(index));
  }
  EXPECT_EQ(observer.ProtocolCode(), std::to_string(static_cast<int>(::grpc::StatusCode::INVALID_ARGUMENT)));
}

TEST(AsyncStreamGrpc, SdkCallbackSubscriptionReportsNativeDeadlineOnce) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  auto sdk = MakeStreamSdk(server);
  SdkStreamObserver observer;
  auto options = SdkStreamOptions();
  options.timeout = kShortStreamDeadline;
  auto stream = sdk->SubscribeTask(SdkSubscriptionRequest(), observer, options);
  ASSERT_TRUE(stream.ok());
  EXPECT_TRUE(observer.WaitEvents(1));
  EXPECT_FALSE(observer.Finished());
  EXPECT_EQ(observer.terminals, 1);
  EXPECT_EQ(observer.ProtocolCode(), std::to_string(static_cast<int>(::grpc::StatusCode::DEADLINE_EXCEEDED)));
}

TEST(AsyncStreamGrpc, SdkHandleCanCancelFromItsOwnCallback) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  auto sdk = MakeStreamSdk(server);
  std::promise<client::StreamHandle*> handle;
  auto available = handle.get_future().share();
  std::promise<void> cancelled;
  auto done = cancelled.get_future();
  SdkStreamObserver observer;
  observer.on_event = [available, &cancelled] {
    available.get()->Cancel();
    cancelled.set_value();
  };
  auto stream = sdk->SubscribeTask(SdkSubscriptionRequest(), observer, SdkStreamOptions());
  ASSERT_TRUE(stream.ok());
  handle.set_value(stream.value().get());
  EXPECT_EQ(done.wait_for(kCoroutineWait), std::future_status::ready);
  EXPECT_TRUE(sdk->Destroy().ok());
  EXPECT_FALSE(stream.value()->IsActive());
  EXPECT_EQ(observer.Count(), 1);
  EXPECT_EQ(observer.terminals, 0);
}

TEST(AsyncStreamGrpc, SdkOwnerCanBeDestroyedFromStreamCallback) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  auto sdk = MakeStreamSdk(server);
  std::promise<void> destroyed;
  auto done = destroyed.get_future();
  std::promise<void> initiated;
  const auto ready = initiated.get_future().share();
  SdkStreamObserver observer;
  observer.on_event = [&, ready] {
    ready.wait();
    sdk.reset();
    destroyed.set_value();
  };
  auto stream = sdk->SubscribeTask(SdkSubscriptionRequest(), observer, SdkStreamOptions());
  initiated.set_value();
  ASSERT_TRUE(stream.ok());
  ASSERT_EQ(done.wait_for(kCoroutineWait), std::future_status::ready);
  stream.value()->Cancel();
  EXPECT_EQ(sdk, nullptr);
  EXPECT_FALSE(stream.value()->IsActive());
  EXPECT_EQ(observer.terminals, 0);
}

TEST(AsyncStreamGrpc, SdkExternalCancellationWaitsForExecutingObserver) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  auto sdk = MakeStreamSdk(server);
  std::promise<void> release;
  const auto allowed = release.get_future().share();
  SdkStreamObserver observer;
  observer.on_event = [allowed] { allowed.wait(); };
  auto stream = sdk->SubscribeTask(SdkSubscriptionRequest(), observer, SdkStreamOptions());
  ASSERT_TRUE(stream.ok());
  const bool entered = observer.WaitEvents(1);
  EXPECT_TRUE(entered);
  auto cancelled = std::async(std::launch::async, [&] { stream.value()->Cancel(); });
  if (entered) {
    EXPECT_EQ(cancelled.wait_for(kPumpInterval), std::future_status::timeout);
  }
  release.set_value();
  EXPECT_EQ(cancelled.wait_for(kCoroutineWait), std::future_status::ready);
  EXPECT_TRUE(sdk->Destroy().ok());
  EXPECT_EQ(observer.terminals, 0);
}

TEST(AsyncStreamGrpc, ThrowingSdkObserverCancelsAndReleasesNativeCall) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  auto sdk = MakeStreamSdk(server);
  SdkStreamObserver observer;
  std::atomic_bool threw = false;
  observer.on_event = [&] {
    threw.store(true);
    throw std::runtime_error(kStreamError);
  };
  auto stream = sdk->SubscribeTask(SdkSubscriptionRequest(), observer, SdkStreamOptions());
  ASSERT_TRUE(stream.ok());
  EXPECT_TRUE(observer.WaitEvents(1));
  EXPECT_TRUE(sdk->Destroy().ok());
  EXPECT_TRUE(threw.load());
  EXPECT_FALSE(stream.value()->IsActive());
}

std::size_t StreamClientThreads() {
#if defined(__linux__)
  return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(kGrpcStreamTaskDirectory), {}));
#else
  return 0;
#endif
}

TEST(AsyncStreamGrpc, SdkIdleSubscriptionsDoNotOwnReaderThreadsAndShutdownDrainsThem) {
  NativeStreamServer server;
  ASSERT_TRUE(server.Ready());
  auto sdk = MakeStreamSdk(server);
  ExpectSdkMessageStream(*sdk, SdkStreamOptions());
  const auto baseline = StreamClientThreads();
  std::vector<std::unique_ptr<SdkStreamObserver>> observers;
  std::vector<std::unique_ptr<client::StreamHandle>> streams;
  for (std::size_t index = 0; index < kStreamConcurrency; ++index) {
    auto observer = std::make_unique<SdkStreamObserver>();
    auto stream = sdk->SubscribeTask(SdkSubscriptionRequest(), *observer, SdkStreamOptions());
    ASSERT_TRUE(stream.ok());
    EXPECT_TRUE(observer->WaitEvents(1));
    observers.push_back(std::move(observer));
    streams.push_back(std::move(stream.value()));
  }
  if (baseline != 0) {
    EXPECT_LT(StreamClientThreads(), baseline + kIdleThreadAllowance);
  }
  EXPECT_TRUE(sdk->Destroy().ok());
  for (const auto& stream : streams) {
    EXPECT_FALSE(stream->IsActive());
  }
  for (const auto& observer : observers) {
    EXPECT_EQ(observer->terminals, 0);
  }
}

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
  ExpectSdkMessageStream(sdk, options);
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
  arguments.SetInt(GRPC_ARG_HTTP2_STREAM_LOOKAHEAD_BYTES, kMinimumStreamLookaheadBytes);
  arguments.SetInt(GRPC_ARG_HTTP2_BDP_PROBE, 0);
  auto stub = lf::a2a::v1::A2AService::NewStub(
      ::grpc::CreateCustomChannel(endpoint, ::grpc::InsecureChannelCredentials(), arguments));
  ::grpc::ClientContext context;
  context.AddMetadata(std::string(GrpcServerTransport::kVersionMetadataKey),
                      std::string(core::Version::kProtocolVersion));
  context.set_deadline(std::chrono::system_clock::now() + kStreamClientTimeout);
  auto reader = stub->SendStreamingMessage(&context, lf::a2a::v1::SendMessageRequest{});
  // Drive client I/O on Windows while leaving the flow-controlled message unread.
  reader->WaitForInitialMetadata();
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
