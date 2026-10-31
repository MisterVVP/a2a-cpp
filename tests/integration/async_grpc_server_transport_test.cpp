// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "a2a/core/extensions.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "support/async_unary_test_support.h"

namespace {
using namespace a2a::test;
using namespace a2a::server;
using namespace a2a::server::engine;
static_assert(!std::is_copy_constructible_v<AsyncGrpcServerTransport>);
static_assert(!std::is_move_constructible_v<AsyncGrpcServerTransport>);
#if defined(GRPC_ARG_ENABLE_DEADLINE_CHECKS)
constexpr bool kCanDisableClientDeadlines = true;
#else
constexpr bool kCanDisableClientDeadlines = false;
#endif
constexpr auto kAuthorization = "authorization";
constexpr auto kRequestHeader = "x-request-id";
constexpr auto kVersionKey = "a2a-version";
constexpr auto kExtensionsKey = "a2a-extensions";
constexpr auto kErrorCodeKey = "a2a-error-code";
constexpr auto kProtocolCodeKey = "a2a-protocol-code";
constexpr auto kDetailsKey = "grpc-status-details-bin";
constexpr auto kBadVersion = "unsupported-version";
constexpr auto kReplyTimeout = "Timed out waiting for native gRPC completion";
constexpr std::size_t kPendingCount = 32;
constexpr std::size_t kWorkers = 2;
constexpr int kRaceParticipants = 3;
constexpr auto kShortDeadline = std::chrono::milliseconds(250);
constexpr auto kDeadlineReplyMargin = std::chrono::milliseconds(5);
constexpr std::array kDeadlineReplyOffsets{-kDeadlineReplyMargin, std::chrono::milliseconds::zero(),
                                           kDeadlineReplyMargin};
constexpr std::size_t kSingleCompletion = 1;

std::string Trailing(const ::grpc::ClientContext& context, const char* key) {
  const auto found = context.GetServerTrailingMetadata().find(key);
  if (found == context.GetServerTrailingMetadata().end()) {
    return {};
  }
  return {found->second.data(), found->second.size()};
}
struct UnaryCase final {
  DispatcherOperation operation;
  std::function<::grpc::Status(lf::a2a::v1::A2AService::Stub&, ::grpc::ClientContext&)> invoke;
};
std::vector<UnaryCase> AllUnaryCases() {
  return {
      {.operation = DispatcherOperation::kSendMessage,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::SendMessageRequest request;
             lf::a2a::v1::SendMessageResponse response;
             return stub.SendMessage(&context, request, &response);
           }},
      {.operation = DispatcherOperation::kGetTask,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::GetTaskRequest request;
             lf::a2a::v1::Task response;
             return stub.GetTask(&context, request, &response);
           }},
      {.operation = DispatcherOperation::kListTasks,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::ListTasksRequest request;
             lf::a2a::v1::ListTasksResponse response;
             return stub.ListTasks(&context, request, &response);
           }},
      {.operation = DispatcherOperation::kCancelTask,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::CancelTaskRequest request;
             lf::a2a::v1::Task response;
             return stub.CancelTask(&context, request, &response);
           }},
      {.operation = DispatcherOperation::kCreateTaskPushNotificationConfig,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::TaskPushNotificationConfig request;
             lf::a2a::v1::TaskPushNotificationConfig response;
             return stub.CreateTaskPushNotificationConfig(&context, request, &response);
           }},
      {.operation = DispatcherOperation::kGetTaskPushNotificationConfig,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::GetTaskPushNotificationConfigRequest request;
             lf::a2a::v1::TaskPushNotificationConfig response;
             return stub.GetTaskPushNotificationConfig(&context, request, &response);
           }},
      {.operation = DispatcherOperation::kListTaskPushNotificationConfigs,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::ListTaskPushNotificationConfigsRequest request;
             lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
             return stub.ListTaskPushNotificationConfigs(&context, request, &response);
           }},
      {.operation = DispatcherOperation::kDeleteTaskPushNotificationConfig,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request;
             google::protobuf::Empty response;
             return stub.DeleteTaskPushNotificationConfig(&context, request, &response);
           }},
      {.operation = DispatcherOperation::kGetExtendedAgentCard,
       .invoke =
           [](auto& stub, auto& context) {
             lf::a2a::v1::GetExtendedAgentCardRequest request;
             lf::a2a::v1::AgentCard response;
             return stub.GetExtendedAgentCard(&context, request, &response);
           }},
  };
}
void PrintTo(const UnaryCase& value, std::ostream* stream) { *stream << static_cast<int>(value.operation); }
class AsyncGrpcUnaryTest : public testing::TestWithParam<UnaryCase> {};
TEST_P(AsyncGrpcUnaryTest, EveryUnaryOperationCompletesWithActivatedExtensions) {
  auto backend = std::make_shared<UnaryBackend>();
  UnaryGrpcServer server(backend, true, {.required_extensions = {kAsyncExtension}});
  ASSERT_TRUE(server.Ready());
  ::grpc::ClientContext context;
  PrepareGrpcClient(context);
  context.AddMetadata(kExtensionsKey, kAsyncExtension);
  EXPECT_TRUE(GetParam().invoke(server.Stub(), context).ok());
  EXPECT_EQ(Trailing(context, kExtensionsKey), kAsyncExtension);
}
TEST_P(AsyncGrpcUnaryTest, VersionValidationPrecedesExecution) {
  auto backend = std::make_shared<UnaryBackend>();
  UnaryGrpcServer server(backend);
  ::grpc::ClientContext context;
  context.AddMetadata(kVersionKey, kBadVersion);
  context.set_deadline(std::chrono::system_clock::now() + kAsyncWait);
  const auto status = GetParam().invoke(server.Stub(), context);
  EXPECT_EQ(status.error_code(), ::grpc::StatusCode::UNIMPLEMENTED);
  EXPECT_EQ(backend->WorkerCount(), 0);
}
INSTANTIATE_TEST_SUITE_P(AllUnaryOperations, AsyncGrpcUnaryTest, testing::ValuesIn(AllUnaryCases()));

struct ClientGet final : std::enable_shared_from_this<ClientGet> {
  ClientGet() : future(reply.get_future()) {
    request.set_id(kAsyncTaskId);
    PrepareGrpcClient(context);
  }
  void Begin(UnaryGrpcServer& server) {
    server.Client().GetTask(server.Stub(), context, request, response, [self = shared_from_this()](auto status) {
      if (self->completions.fetch_add(1) == 0) {
        self->reply.set_value(std::move(status));
      }
    });
  }
  ::grpc::Status Await() {
    if (future.wait_for(kAsyncWait) != std::future_status::ready) {
      context.TryCancel();
      ADD_FAILURE() << kReplyTimeout;
      return {::grpc::StatusCode::DEADLINE_EXCEEDED, kReplyTimeout};
    }
    return future.get();
  }
  ::grpc::ClientContext context;
  lf::a2a::v1::GetTaskRequest request;
  lf::a2a::v1::Task response;
  std::promise<::grpc::Status> reply;
  std::future<::grpc::Status> future;
  std::atomic<std::size_t> completions{0};
};

class CountAfter final : public ServerInterceptor {
 public:
  void AfterDispatch(const DispatchRequest& /*request*/, RequestContext& /*context*/,
                     const a2a::core::Result<DispatchResponse>& /*result*/) override {
    {
      std::lock_guard lock(mutex_);
      ++count_;
    }
    ready_.notify_all();
  }
  [[nodiscard]] bool Wait(std::size_t count) {
    std::unique_lock lock(mutex_);
    return ready_.wait_for(lock, kAsyncWait, [&] { return count_ == count; });
  }
  [[nodiscard]] std::size_t Count() const {
    std::lock_guard lock(mutex_);
    return count_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  std::size_t count_ = 0;
};

TEST(AsyncGrpcTest, OwnsMetadataAndConvertsDeadlineAndHistory) {
  auto backend = std::make_shared<UnaryBackend>();
  UnaryGrpcServer server(backend);
  auto call = std::make_shared<ClientGet>();
  call->context.AddMetadata(kAuthorization, kAsyncAuth);
  call->context.AddMetadata(kRequestHeader, kAsyncRequestId);
  call->request.set_history_length(1);
  call->Begin(server);
  ASSERT_TRUE(call->Await().ok());
  const auto execution = backend->Context();
  EXPECT_EQ(execution.Metadata().auth_metadata.at(kAuthorization), kAsyncAuth);
  EXPECT_EQ(execution.Metadata().client_headers.at(kRequestHeader), kAsyncRequestId);
  EXPECT_TRUE(execution.Metadata().remote_address.has_value());
  EXPECT_EQ(execution.Metadata().protocol_version, a2a::core::Version::kProtocolVersion);
  EXPECT_TRUE(execution.Deadline().has_value());
  EXPECT_EQ(call->response.history_size(), 1);
}
TEST(AsyncGrpcTest, RequiredExtensionErrorsMatchSyncTransport) {
  auto backend = std::make_shared<UnaryBackend>();
  UnaryGrpcServer asynchronous(backend, true, {.required_extensions = {kAsyncExtension}});
  UnaryGrpcServer synchronous(backend, false, {.required_extensions = {kAsyncExtension}});
  ::grpc::ClientContext async_context;
  ::grpc::ClientContext sync_context;
  PrepareGrpcClient(async_context);
  PrepareGrpcClient(sync_context);
  lf::a2a::v1::GetTaskRequest request;
  lf::a2a::v1::Task reply;
  auto async_status = asynchronous.Stub().GetTask(&async_context, request, &reply);
  auto sync_status = synchronous.Stub().GetTask(&sync_context, request, &reply);
  EXPECT_EQ(async_status.error_code(), sync_status.error_code());
  EXPECT_EQ(async_status.error_message(), sync_status.error_message());
  EXPECT_EQ(Trailing(async_context, kDetailsKey), Trailing(sync_context, kDetailsKey));
}
TEST(AsyncGrpcTest, ApplicationErrorDetailsAndProtocolCodesMatchSyncTransport) {
  auto backend = std::make_shared<UnaryBackend>();
  UnaryGrpcServer asynchronous(backend);
  UnaryGrpcServer synchronous(backend, false);
  ::grpc::ClientContext async_context;
  ::grpc::ClientContext sync_context;
  PrepareGrpcClient(async_context);
  PrepareGrpcClient(sync_context);
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kMissingTaskId);
  lf::a2a::v1::Task reply;
  EXPECT_EQ(asynchronous.Stub().GetTask(&async_context, request, &reply).error_code(), ::grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(synchronous.Stub().GetTask(&sync_context, request, &reply).error_code(), ::grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(Trailing(async_context, kErrorCodeKey), Trailing(sync_context, kErrorCodeKey));
  EXPECT_EQ(Trailing(async_context, kProtocolCodeKey), a2a::core::protocol_codes::kTaskNotFound);
  EXPECT_EQ(Trailing(async_context, kDetailsKey), Trailing(sync_context, kDetailsKey));
}
TEST(AsyncGrpcTest, ListValidationAndProjectionUseSharedConversion) {
  auto backend = std::make_shared<UnaryBackend>();
  UnaryGrpcServer server(backend);
  ::grpc::ClientContext context;
  PrepareGrpcClient(context);
  lf::a2a::v1::ListTasksRequest request;
  request.set_history_length(-1);
  lf::a2a::v1::ListTasksResponse response;
  EXPECT_EQ(server.Stub().ListTasks(&context, request, &response).error_code(), ::grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(backend->WorkerCount(), 0);
}
TEST(AsyncGrpcTest, ManyPendingRequestsUseFixedApplicationWorkers) {
  auto backend = std::make_shared<UnaryBackend>(true);
  AsyncDispatcherOptions options;
  UnaryGrpcServer server(backend, true, {}, options);
  std::vector<std::shared_ptr<ClientGet>> calls;
  for (std::size_t index = 0; index < kPendingCount; ++index) {
    auto call = std::make_shared<ClientGet>();
    call->Begin(server);
    calls.push_back(std::move(call));
  }
  ASSERT_TRUE(backend->WaitForCalls(kPendingCount));
  EXPECT_LE(backend->WorkerCount(), kWorkers);
  backend->Release();
  for (const auto& call : calls) {
    EXPECT_TRUE(call->Await().ok());
  }
}
TEST(AsyncGrpcTest, ClientCancellationSignalsApplicationAndAfterExactlyOnce) {
  auto backend = std::make_shared<UnaryBackend>(true);
  auto after = std::make_shared<CountAfter>();
  AsyncDispatcherOptions options;
  options.interceptors = {after};
  UnaryGrpcServer server(backend, true, {}, options);
  auto call = std::make_shared<ClientGet>();
  call->Begin(server);
  ASSERT_TRUE(backend->WaitForCalls(1));
  call->context.TryCancel();
  EXPECT_EQ(call->Await().error_code(), ::grpc::StatusCode::CANCELLED);
  ASSERT_TRUE(backend->WaitForStops(1));
  EXPECT_EQ(backend->Stopped().front(), RequestOutcome::kClientCancelled);
  EXPECT_TRUE(after->Wait(1));
  backend->Release();
}
::grpc::ChannelArguments DeadlineClientOptions() {
  ::grpc::ChannelArguments options;
#if defined(GRPC_ARG_ENABLE_DEADLINE_CHECKS)
  options.SetInt(GRPC_ARG_ENABLE_DEADLINE_CHECKS, 0);
#endif
  return options;
}

bool IsDeadlineTerminal(RequestOutcome outcome) {
  return outcome == RequestOutcome::kDeadlineExceeded ||
         (!kCanDisableClientDeadlines && outcome == RequestOutcome::kClientCancelled);
}

TEST(AsyncGrpcTest, NativeDeadlinePublishesTheObservedTerminalSignal) {
  auto backend = std::make_shared<UnaryBackend>(true);
  // Older gRPC can isolate the server timer. Newer releases always enforce
  // client deadlines, which can cancel before the rounded server wire deadline.
  const auto client_options = DeadlineClientOptions();
  UnaryGrpcServer server(backend, true, {}, {}, client_options);
  auto call = std::make_shared<ClientGet>();
  call->context.set_deadline(std::chrono::system_clock::now() + kShortDeadline);
  call->Begin(server);
  ASSERT_TRUE(backend->WaitForCalls(1));
  EXPECT_EQ(call->Await().error_code(), ::grpc::StatusCode::DEADLINE_EXCEEDED);
  ASSERT_TRUE(backend->WaitForStops(1));
  EXPECT_TRUE(IsDeadlineTerminal(backend->Stopped().front()));
  backend->Release();
}

bool WarmDeadlineRaceChannel(UnaryGrpcServer& server) {
  // A version rejection warms the real channel without entering the dispatcher
  // or changing the application's call and interceptor counters.
  ::grpc::ClientContext context;
  context.AddMetadata(kVersionKey, kBadVersion);
  context.set_deadline(std::chrono::system_clock::now() + kAsyncWait);
  lf::a2a::v1::GetTaskRequest request;
  lf::a2a::v1::Task response;
  return server.Stub().GetTask(&context, request, &response).error_code() == ::grpc::StatusCode::UNIMPLEMENTED;
}

void ExpectDeadlineRaceOutcome(const ::grpc::Status& status, RequestOutcome outcome) {
  EXPECT_TRUE(status.ok() || status.error_code() == ::grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_TRUE(outcome == RequestOutcome::kSucceeded || IsDeadlineTerminal(outcome));
  if (status.ok()) {
    EXPECT_EQ(outcome, RequestOutcome::kSucceeded);
  }
  // Wire expiry can follow an already committed server success. It cannot turn
  // a server deadline failure into a successful client response.
  if (IsDeadlineTerminal(outcome)) {
    EXPECT_EQ(status.error_code(), ::grpc::StatusCode::DEADLINE_EXCEEDED);
  }
}

void ExpectDeadlineRaceFinalization(const CountAfter& after, const ClientGet& call,
                                    const RequestExecutionContext& execution, RequestOutcome outcome) {
  EXPECT_EQ(after.Count(), kSingleCompletion);
  EXPECT_EQ(call.completions.load(), kSingleCompletion);
  EXPECT_EQ(execution.Outcome(), outcome);
}

void RunDeadlineCompletionRace(std::chrono::milliseconds reply_offset) {
  auto backend = std::make_shared<UnaryBackend>(true);
  auto after = std::make_shared<CountAfter>();
  auto call = std::make_shared<ClientGet>();
  std::optional<RequestExecutionContext> execution;
  UnaryBackend::PendingResult late_reply;
  ::grpc::Status status;
  {
    AsyncDispatcherOptions options;
    options.interceptors = {after};
    const auto client_options = DeadlineClientOptions();
    UnaryGrpcServer server(backend, true, {}, options, client_options);
    ASSERT_TRUE(server.Ready());
    ASSERT_TRUE(WarmDeadlineRaceChannel(server));
    call->context.set_deadline(std::chrono::system_clock::now() + kShortDeadline);
    call->Begin(server);
    ASSERT_TRUE(backend->WaitForCalls(kSingleCompletion));
    execution = backend->Context();
    const auto deadline = execution->Deadline();
    ASSERT_TRUE(deadline.has_value());
    const auto reply_at = deadline.value_or(RequestExecution::Clock::time_point{}) + reply_offset;
    late_reply = backend->PendingReply();
    // Race real application completion against native deadline delivery without
    // manually cancelling the client or expiring the SDK lifecycle source.
    std::thread application([backend, reply_at] {
      std::this_thread::sleep_until(reply_at);
      backend->Release();
    });
    status = call->Await();
    application.join();
    ASSERT_TRUE(after->Wait(kSingleCompletion));
    server.Shutdown();
    late_reply->Resolve(AsyncTask());  // OnDone has released the reactor and its response.
  }  // Drain the Asio runtime before inspecting exactly-once counts.
  const auto outcome = execution->Outcome();
  late_reply->Resolve(AsyncTask());  // Also harmless after dispatcher/service destruction.
  ExpectDeadlineRaceOutcome(status, outcome);
  ExpectDeadlineRaceFinalization(*after, *call, *execution, outcome);
}

TEST(AsyncGrpcTest, NativeDeadlineVersusCompletionRaceRunsAfterOnceAndIgnoresLateReplies) {
  for (const auto offset : kDeadlineReplyOffsets) {
    SCOPED_TRACE(offset.count());
    RunDeadlineCompletionRace(offset);
  }
}

TEST(AsyncGrpcTest, ShutdownCompletesCallsWithoutWaitingForBackendAndRejectsAdmission) {
  auto backend = std::make_shared<UnaryBackend>(true);
  UnaryGrpcServer server(backend);
  auto call = std::make_shared<ClientGet>();
  call->Begin(server);
  ASSERT_TRUE(backend->WaitForCalls(1));
  server.CloseAdmission();
  EXPECT_FALSE(call->Await().ok());
  ASSERT_TRUE(backend->WaitForStops(1));
  EXPECT_EQ(backend->Stopped().front(), RequestOutcome::kServerShutdown);
  auto late = std::make_shared<ClientGet>();
  late->Begin(server);
  EXPECT_EQ(late->Await().error_code(), ::grpc::StatusCode::UNAVAILABLE);
  server.Shutdown();
  backend->Release();  // Saved executor replies are safe even after gRPC OnDone.
}
TEST(AsyncGrpcTest, CancelVersusCompletionRaceStillRunsAfterOnce) {
  auto backend = std::make_shared<UnaryBackend>(true);
  auto after = std::make_shared<CountAfter>();
  AsyncDispatcherOptions options;
  options.interceptors = {after};
  UnaryGrpcServer server(backend, true, {}, options);
  auto call = std::make_shared<ClientGet>();
  call->Begin(server);
  ASSERT_TRUE(backend->WaitForCalls(1));
  std::barrier race(kRaceParticipants);
  std::thread application([&] {
    race.arrive_and_wait();
    backend->Release();
  });
  std::thread cancellation([&] {
    race.arrive_and_wait();
    call->context.TryCancel();
  });
  race.arrive_and_wait();
  application.join();
  cancellation.join();
  const auto status = call->Await();
  EXPECT_TRUE(status.ok() || status.error_code() == ::grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(after->Wait(1));
}
TEST(AsyncGrpcTest, StreamsAreExplicitlyUnimplementedOnCallbackService) {
  UnaryGrpcServer server(std::make_shared<UnaryBackend>());
  ::grpc::ClientContext context;
  PrepareGrpcClient(context);
  lf::a2a::v1::SendMessageRequest request;
  auto stream = server.Stub().SendStreamingMessage(&context, request);
  lf::a2a::v1::StreamResponse response;
  EXPECT_FALSE(stream->Read(&response));
  EXPECT_EQ(stream->Finish().error_code(), ::grpc::StatusCode::UNIMPLEMENTED);
}
TEST(AsyncGrpcTest, NullDispatcherIsRejectedAtConfiguration) {
  EXPECT_THROW(AsyncGrpcServerTransport(nullptr), std::invalid_argument);
}

TEST(AsyncGrpcTest, ImmediateCancellationCanRaceAdmissionAndSourceBinding) {
  auto backend = std::make_shared<UnaryBackend>(true);
  UnaryGrpcServer server(backend);
  std::vector<std::shared_ptr<ClientGet>> calls;
  for (std::size_t index = 0; index < kPendingCount; ++index) {
    auto call = std::make_shared<ClientGet>();
    call->Begin(server);
    call->context.TryCancel();
    calls.push_back(std::move(call));
  }
  for (const auto& call : calls) {
    EXPECT_EQ(call->Await().error_code(), ::grpc::StatusCode::CANCELLED);
  }
  server.Shutdown();
  backend->Release();
}

constexpr auto kAuthenticatedIdentity = "native-user";
class NativePolicy final : public SecurityPolicy {
 public:
  [[nodiscard]] a2a::core::Result<std::string> Authenticate(const RequestContext& context) const override {
    const auto found = context.auth_metadata.find(kAuthorization);
    if (found == context.auth_metadata.end() || found->second != kAsyncAuth) {
      return a2a::core::protocol_errors::Unauthenticated();
    }
    return std::string(kAuthenticatedIdentity);
  }
  [[nodiscard]] bool AuthorizeOperation(const DispatchRequest& /*request*/,
                                        const RequestContext& context) const override {
    return context.principal == kAuthenticatedIdentity;
  }
  [[nodiscard]] bool CanAccessTask(const lf::a2a::v1::Task& /*task*/, const RequestContext& context) const override {
    return context.principal == kAuthenticatedIdentity;
  }
};
TEST(AsyncGrpcTest, SecurityAuthenticatesCopiedMetadataAndProjectsAuthorizedHistory) {
  auto backend = std::make_shared<UnaryBackend>();
  InMemoryTaskStore store;
  ASSERT_TRUE(store.CreateOrUpdate(AsyncTask()).ok());
  AsyncDispatcherOptions options;
  options.security_policy = std::make_shared<NativePolicy>();
  options.task_store = &store;
  UnaryGrpcServer server(backend, true, {}, options);
  auto denied = std::make_shared<ClientGet>();
  denied->Begin(server);
  EXPECT_EQ(denied->Await().error_code(), ::grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(backend->WorkerCount(), 0);
  auto allowed = std::make_shared<ClientGet>();
  allowed->context.AddMetadata(kAuthorization, kAsyncAuth);
  allowed->request.set_history_length(1);
  allowed->Begin(server);
  ASSERT_TRUE(allowed->Await().ok());
  EXPECT_EQ(backend->Context().Metadata().principal, kAuthenticatedIdentity);
  EXPECT_EQ(allowed->response.history_size(), 1);
}
TEST(AsyncGrpcTest, CapacityExhaustionReturnsUnavailableWithoutStartingApplication) {
  auto backend = std::make_shared<UnaryBackend>(true);
  AsyncDispatcherOptions options;
  options.max_in_flight = 1;
  UnaryGrpcServer server(backend, true, {}, options);
  auto first = std::make_shared<ClientGet>();
  first->Begin(server);
  ASSERT_TRUE(backend->WaitForCalls(1));
  auto overflow = std::make_shared<ClientGet>();
  overflow->Begin(server);
  EXPECT_EQ(overflow->Await().error_code(), ::grpc::StatusCode::UNAVAILABLE);
  first->context.TryCancel();
  EXPECT_EQ(first->Await().error_code(), ::grpc::StatusCode::CANCELLED);
  backend->Release();
}
TEST(AsyncGrpcTest, InfiniteNativeDeadlineRemainsAbsentInExecutionContext) {
  auto backend = std::make_shared<UnaryBackend>();
  UnaryGrpcServer server(backend);
  ::grpc::ClientContext context;
  context.AddMetadata(kVersionKey, std::string(a2a::core::Version::kProtocolVersion));
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kAsyncTaskId);
  lf::a2a::v1::Task response;
  ASSERT_TRUE(server.Stub().GetTask(&context, request, &response).ok());
  EXPECT_FALSE(backend->Context().Deadline().has_value());
}
TEST(AsyncGrpcTest, AlreadyExpiredClientDeadlineDoesNotStartApplication) {
  auto backend = std::make_shared<UnaryBackend>();
  UnaryGrpcServer server(backend);
  auto call = std::make_shared<ClientGet>();
  call->context.set_deadline(std::chrono::system_clock::time_point{});
  call->Begin(server);
  EXPECT_EQ(call->Await().error_code(), ::grpc::StatusCode::DEADLINE_EXCEEDED);
  EXPECT_EQ(backend->WorkerCount(), 0);
}
}  // namespace
