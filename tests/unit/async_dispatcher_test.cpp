// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/engine/async_dispatcher.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "a2a/core/extensions.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/server/tasks/in_memory_task_store.h"

namespace {
using namespace a2a::server;
using namespace a2a::server::engine;
using a2a::core::Error;
using a2a::core::Result;
constexpr auto kTaskId = "task-one";
constexpr auto kRequestId = "request-one";
constexpr auto kInterceptedId = "intercepted-request";
constexpr auto kFailure = "executor-failure";
constexpr auto kMissingContext = "Executor did not retain its lifecycle context";
constexpr auto kCompletionTimeout = "Timed out waiting for dispatcher completion";
constexpr auto kBefore = "before";
constexpr auto kAfter = "after";
constexpr auto kExecute = "execute";
constexpr auto kExtension = "https://example.com/extension";
constexpr auto kPrincipal = "authenticated-principal";
constexpr auto kTenant = "tenant-one";
constexpr auto kCardName = "private-card";
constexpr auto kWait = std::chrono::seconds(5);
constexpr std::size_t kConcurrency = 32;
constexpr std::size_t kWorkerCount = 2;
constexpr int kRaceParties = 3;
constexpr int kHistorySize = 3;
constexpr int kHistoryLimit = 1;

lf::a2a::v1::Task MakeTask() {
  lf::a2a::v1::Task task;
  task.set_id(kTaskId);
  for (int index = 0; index < kHistorySize; ++index) {
    task.add_history()->set_task_id(kTaskId);
  }
  return task;
}
DispatchRequest GetTaskRequest() {
  lf::a2a::v1::GetTaskRequest query;
  query.set_id(kTaskId);
  return {.operation = DispatcherOperation::kGetTask, .payload = std::move(query)};
}

struct Trace final {
  void Add(const char* entry) {
    std::lock_guard lock(mutex);
    entries.emplace_back(entry);
  }
  std::vector<std::string> Read() {
    std::lock_guard lock(mutex);
    return entries;
  }
  std::mutex mutex;
  std::vector<std::string> entries;
};
class RecordingInterceptor final : public ServerInterceptor {
 public:
  explicit RecordingInterceptor(std::shared_ptr<Trace> trace) : trace_(std::move(trace)) {}
  Result<void> BeforeDispatch(const DispatchRequest& /*request*/, RequestContext& context) override {
    trace_->Add(kBefore);
    context.request_id = kInterceptedId;
    context.principal = kInterceptedId;
    if (reject) {
      return Error::Validation(kFailure);
    }
    return {};
  }
  void AfterDispatch(const DispatchRequest& /*request*/, RequestContext& context,
                     const Result<DispatchResponse>& result) override {
    trace_->Add(kAfter);
    ++after_count;
    after_ok = result.ok();
    after_principal = context.principal;
    if (throw_after) {
      throw std::runtime_error(kFailure);
    }
  }
  bool reject = false;
  bool throw_after = false;
  std::atomic<int> after_count{0};
  bool after_ok = false;
  std::string after_principal;

 private:
  std::shared_ptr<Trace> trace_;
};

class FakeExecutor : public AsyncAgentExecutor {
 public:
  void Record(const RequestExecutionContext& context) {
    if (trace != nullptr) {
      trace->Add(kExecute);
    }
    last_metadata = context.Metadata();
    last_context = context;
    ++calls;
  }
  void SendMessage(lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context,
                   Completion<lf::a2a::v1::SendMessageResponse> completion) override {
    Record(context);
    send_history_limit = request.configuration().has_history_length() ? request.configuration().history_length() : -1;
    if (fail) {
      completion(Error::Validation(kFailure));
      return;
    }
    lf::a2a::v1::SendMessageResponse response;
    *response.mutable_task() = MakeTask();
    completion(std::move(response));
  }
  void GetTask(lf::a2a::v1::GetTaskRequest request, RequestExecutionContext context,
               Completion<lf::a2a::v1::Task> completion) override {
    Record(context);
    get_history_limit = request.has_history_length() ? request.history_length() : -1;
    if (pending) {
      held.set_value(std::move(completion));
      return;
    }
    if (fail) {
      completion(Error::Validation(kFailure));
      return;
    }
    auto response = MakeTask();
    completion(std::move(response));
  }
  void ListTasks(ListTasksRequest /*request*/, RequestExecutionContext context,
                 Completion<ListTasksResponse> completion) override {
    Record(context);
    if (fail) {
      completion(Error::Validation(kFailure));
      return;
    }
    ListTasksResponse response;
    response.tasks.push_back(MakeTask());
    completion(std::move(response));
  }
  void CancelTask(lf::a2a::v1::CancelTaskRequest /*request*/, RequestExecutionContext context,
                  Completion<lf::a2a::v1::Task> completion) override {
    Record(context);
    if (fail) {
      completion(Error::Validation(kFailure));
      return;
    }
    auto response = MakeTask();
    completion(std::move(response));
  }
  void CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig /*request*/,
                                        RequestExecutionContext context,
                                        Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) override {
    Record(context);
    if (fail) {
      completion(Error::Validation(kFailure));
      return;
    }
    lf::a2a::v1::TaskPushNotificationConfig response;
    response.set_task_id(kTaskId);
    completion(std::move(response));
  }
  void GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest /*request*/,
                                     RequestExecutionContext context,
                                     Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) override {
    Record(context);
    if (fail) {
      completion(Error::Validation(kFailure));
      return;
    }
    lf::a2a::v1::TaskPushNotificationConfig response;
    response.set_task_id(kTaskId);
    completion(std::move(response));
  }
  void ListTaskPushNotificationConfigs(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest /*request*/, RequestExecutionContext context,
      Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion) override {
    Record(context);
    if (fail) {
      completion(Error::Validation(kFailure));
      return;
    }
    lf::a2a::v1::ListTaskPushNotificationConfigsResponse response;
    completion(std::move(response));
  }
  void DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest /*request*/,
                                        RequestExecutionContext context, Completion<void> completion) override {
    Record(context);
    if (fail) {
      completion(Error::Validation(kFailure));
      return;
    }
    Result<void> response;
    completion(std::move(response));
  }
  std::shared_ptr<Trace> trace;
  std::atomic<int> calls{0};
  bool fail = false;
  bool pending = false;
  int get_history_limit = 0;
  int send_history_limit = 0;
  RequestContext last_metadata;
  std::optional<RequestExecutionContext> last_context;
  std::promise<Completion<lf::a2a::v1::Task>> held;
};

// Fake transport owns the reply state independently of its stack lifetime.
class FakeTransport final {
 public:
  struct Reply final {
    std::promise<Result<DispatchResponse>> terminal;
    std::atomic<int> count{0};
    std::thread::id thread;
    RequestOutcome outcome = RequestOutcome::kPending;
  };
  FakeTransport() : result_(reply_->terminal.get_future()) {}
  Result<std::shared_ptr<RequestExecution>> Send(AsyncDispatcher& dispatcher,
                                                 DispatchRequest request = GetTaskRequest(),
                                                 RequestContext context = {},
                                                 std::optional<RequestExecution::Clock::time_point> deadline = {}) {
    return dispatcher.Dispatch(
        std::move(request), std::move(context),
        [reply = reply_](auto response, RequestExecutionContext execution) {
          if (reply->count.fetch_add(1) == 0) {
            reply->thread = std::this_thread::get_id();
            reply->outcome = execution.Outcome();
            reply->terminal.set_value(std::move(response));
          }
        },
        deadline);
  }
  Result<DispatchResponse> Receive() {
    if (result_.wait_for(kWait) != std::future_status::ready) {
      ADD_FAILURE() << kCompletionTimeout;
      return Error::Internal(kFailure);
    }
    return result_.get();
  }
  [[nodiscard]] const std::shared_ptr<Reply>& reply() const { return reply_; }

 private:
  std::shared_ptr<Reply> reply_ = std::make_shared<Reply>();
  std::future<Result<DispatchResponse>> result_;
};

struct OperationCase final {
  DispatchRequest request;
  std::size_t response_index;
};
std::vector<OperationCase> UnaryCases() {
  return {
      {.request = {.operation = DispatcherOperation::kSendMessage, .payload = lf::a2a::v1::SendMessageRequest{}},
       .response_index = DispatchResponse(lf::a2a::v1::SendMessageResponse{}).payload().index()},
      {.request = {.operation = DispatcherOperation::kGetTask, .payload = lf::a2a::v1::GetTaskRequest{}},
       .response_index = DispatchResponse(lf::a2a::v1::Task{}).payload().index()},
      {.request = {.operation = DispatcherOperation::kListTasks, .payload = ListTasksRequest{}},
       .response_index = DispatchResponse(ListTasksResponse{}).payload().index()},
      {.request = {.operation = DispatcherOperation::kCancelTask, .payload = lf::a2a::v1::CancelTaskRequest{}},
       .response_index = DispatchResponse(lf::a2a::v1::Task{}).payload().index()},
      {.request = {.operation = DispatcherOperation::kCreateTaskPushNotificationConfig,
                   .payload = lf::a2a::v1::TaskPushNotificationConfig{}},
       .response_index = DispatchResponse(lf::a2a::v1::TaskPushNotificationConfig{}).payload().index()},
      {.request = {.operation = DispatcherOperation::kGetTaskPushNotificationConfig,
                   .payload = lf::a2a::v1::GetTaskPushNotificationConfigRequest{}},
       .response_index = DispatchResponse(lf::a2a::v1::TaskPushNotificationConfig{}).payload().index()},
      {.request = {.operation = DispatcherOperation::kListTaskPushNotificationConfigs,
                   .payload = lf::a2a::v1::ListTaskPushNotificationConfigsRequest{}},
       .response_index = DispatchResponse(lf::a2a::v1::ListTaskPushNotificationConfigsResponse{}).payload().index()},
      {.request = {.operation = DispatcherOperation::kDeleteTaskPushNotificationConfig,
                   .payload = lf::a2a::v1::DeleteTaskPushNotificationConfigRequest{}},
       .response_index = DispatchResponse{}.payload().index()},
      {.request = {.operation = DispatcherOperation::kGetExtendedAgentCard,
                   .payload = lf::a2a::v1::GetExtendedAgentCardRequest{}},
       .response_index = DispatchResponse(lf::a2a::v1::AgentCard{}).payload().index()},
  };
}
void PrintTo(const OperationCase& value, std::ostream* stream) { *stream << static_cast<int>(value.request.operation); }
class AsyncUnaryTest : public testing::TestWithParam<OperationCase> {};
TEST_P(AsyncUnaryTest, RoutesEveryOperationOffCallerThread) {
  auto executor = std::make_shared<FakeExecutor>();
  AsyncDispatcherOptions options;
  lf::a2a::v1::AgentCard card;
  card.set_name(kCardName);
  options.agent_card_provider = std::make_shared<a2a::core::StaticAgentCardProvider>(card);
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  auto accepted = transport.Send(dispatcher, GetParam().request);
  ASSERT_TRUE(accepted.ok());
  auto response = transport.Receive();
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(response.value().payload().index(), GetParam().response_index);
  EXPECT_NE(transport.reply()->thread, std::this_thread::get_id());
  EXPECT_EQ(accepted.value()->Context().Outcome(), RequestOutcome::kSucceeded);
  EXPECT_EQ(transport.reply()->outcome, RequestOutcome::kSucceeded);
}
TEST_P(AsyncUnaryTest, RejectsMismatchedPayloadBeforeExecution) {
  auto executor = std::make_shared<FakeExecutor>();
  AsyncDispatcher dispatcher(executor);
  auto request = GetParam().request;
  // Choose a payload which is never the valid type for this operation.
  if (std::holds_alternative<ListTasksRequest>(request.payload)) {
    request.payload = lf::a2a::v1::GetTaskRequest{};
  } else {
    request.payload = ListTasksRequest{};
  }
  FakeTransport transport;
  ASSERT_TRUE(transport.Send(dispatcher, request).ok());
  auto response = transport.Receive();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), a2a::core::ErrorCode::kValidation);
  EXPECT_EQ(executor->calls.load(), 0);
}
TEST_P(AsyncUnaryTest, PreservesExecutorOrProviderErrorsAndRunsAfterOnce) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->fail = true;
  auto interceptor = std::make_shared<RecordingInterceptor>(std::make_shared<Trace>());
  AsyncDispatcherOptions options;
  options.interceptors = {interceptor};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  ASSERT_TRUE(transport.Send(dispatcher, GetParam().request).ok());
  EXPECT_FALSE(transport.Receive().ok());
  EXPECT_EQ(interceptor->after_count.load(), 1);
  EXPECT_FALSE(interceptor->after_ok);
}
INSTANTIATE_TEST_SUITE_P(UnaryOperations, AsyncUnaryTest, testing::ValuesIn(UnaryCases()));

TEST(AsyncDispatcherTest, InterceptorsObserveMutatedMetadataAndExactlyOneTerminalResult) {
  auto trace = std::make_shared<Trace>();
  auto executor = std::make_shared<FakeExecutor>();
  executor->trace = trace;
  auto interceptor = std::make_shared<RecordingInterceptor>(trace);
  AsyncDispatcherOptions options;
  options.interceptors = {interceptor};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  RequestContext context;
  context.request_id = kRequestId;
  const auto accepted = transport.Send(dispatcher, GetTaskRequest(), context);
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(transport.Receive().ok());
  EXPECT_EQ(trace->Read(), (std::vector<std::string>{kBefore, kExecute, kAfter}));
  EXPECT_EQ(executor->last_metadata.request_id, kInterceptedId);
  EXPECT_EQ(accepted.value()->Context().Metadata().request_id, kRequestId);
  EXPECT_EQ(interceptor->after_count.load(), 1);
}
TEST(AsyncDispatcherTest, RejectedBeforeSkipsExecutorAndStillRunsAfter) {
  auto executor = std::make_shared<FakeExecutor>();
  auto interceptor = std::make_shared<RecordingInterceptor>(std::make_shared<Trace>());
  interceptor->reject = true;
  AsyncDispatcherOptions options;
  options.interceptors = {interceptor};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  ASSERT_TRUE(transport.Send(dispatcher).ok());
  EXPECT_FALSE(transport.Receive().ok());
  EXPECT_EQ(executor->calls.load(), 0);
  EXPECT_EQ(interceptor->after_count.load(), 1);
}
TEST(AsyncDispatcherTest, CancellationCompletesWithoutWaitingForApplicationAndIgnoresLateReplies) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->pending = true;
  auto held = executor->held.get_future();
  AsyncDispatcher dispatcher(executor);
  FakeTransport transport;
  const auto accepted = transport.Send(dispatcher);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(held.wait_for(kWait), std::future_status::ready);
  auto complete = held.get();
  EXPECT_TRUE(accepted.value()->Cancel());
  EXPECT_FALSE(transport.Receive().ok());
  EXPECT_EQ(accepted.value()->Context().Outcome(), RequestOutcome::kClientCancelled);
  EXPECT_EQ(transport.reply()->outcome, RequestOutcome::kClientCancelled);
  const auto observed = executor->last_context;
  if (!observed.has_value()) {
    ADD_FAILURE() << kMissingContext;
    return;
  }
  EXPECT_TRUE(observed->StopToken().stop_requested());
  complete(MakeTask());
  complete(Error::Validation(kFailure));
  EXPECT_EQ(transport.reply()->count.load(), 1);
}
TEST(AsyncDispatcherTest, ExpiredDeadlineSkipsExecutorAndRunsAfter) {
  auto executor = std::make_shared<FakeExecutor>();
  auto interceptor = std::make_shared<RecordingInterceptor>(std::make_shared<Trace>());
  AsyncDispatcherOptions options;
  options.interceptors = {interceptor};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  const auto accepted = transport.Send(dispatcher, GetTaskRequest(), {}, RequestExecution::Clock::time_point{});
  ASSERT_TRUE(accepted.ok());
  EXPECT_FALSE(transport.Receive().ok());
  EXPECT_EQ(accepted.value()->Context().Outcome(), RequestOutcome::kDeadlineExceeded);
  EXPECT_EQ(executor->calls.load(), 0);
  EXPECT_EQ(interceptor->after_count.load(), 1);
}
TEST(AsyncDispatcherTest, ActiveDeadlineSignalWinsOverLateSuccess) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->pending = true;
  auto held = executor->held.get_future();
  AsyncDispatcher dispatcher(executor);
  FakeTransport transport;
  const auto deadline = RequestExecution::Clock::now() + kWait;
  const auto accepted = transport.Send(dispatcher, GetTaskRequest(), {}, deadline);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(held.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(accepted.value()->ExpireDeadline(deadline));
  EXPECT_FALSE(transport.Receive().ok());
  held.get()(MakeTask());
  EXPECT_EQ(accepted.value()->Context().Outcome(), RequestOutcome::kDeadlineExceeded);
}
TEST(AsyncDispatcherTest, CompletionVersusCancellationDeliversOnce) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->pending = true;
  auto held = executor->held.get_future();
  auto interceptor = std::make_shared<RecordingInterceptor>(std::make_shared<Trace>());
  AsyncDispatcherOptions options;
  options.interceptors = {interceptor};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  const auto accepted = transport.Send(dispatcher);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(held.wait_for(kWait), std::future_status::ready);
  auto complete = held.get();
  std::barrier race(kRaceParties);
  std::thread application([&] {
    race.arrive_and_wait();
    complete(MakeTask());
  });
  std::thread transport_cancel([&] {
    race.arrive_and_wait();
    (void)accepted.value()->Cancel();
  });
  race.arrive_and_wait();
  application.join();
  transport_cancel.join();
  const auto response = transport.Receive();
  EXPECT_EQ(response.ok(), accepted.value()->Context().Outcome() == RequestOutcome::kSucceeded);
  EXPECT_EQ(transport.reply()->count.load(), 1);
  EXPECT_EQ(interceptor->after_count.load(), 1);
}
TEST(AsyncDispatcherTest, ShutdownDrainsCallbacksAndReleasesExecutorWithoutApplicationReply) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->pending = true;
  auto held = executor->held.get_future();
  auto dispatcher = std::make_unique<AsyncDispatcher>(executor);
  FakeTransport transport;
  const auto accepted = transport.Send(*dispatcher);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(held.wait_for(kWait), std::future_status::ready);
  auto complete = held.get();
  const auto observed = executor->last_context;
  if (!observed.has_value()) {
    ADD_FAILURE() << kMissingContext;
    return;
  }
  std::weak_ptr<FakeExecutor> weak = executor;
  executor.reset();
  dispatcher.reset();
  EXPECT_EQ(transport.reply()->count.load(), 1);
  EXPECT_FALSE(transport.Receive().ok());
  EXPECT_TRUE(weak.expired());
  EXPECT_EQ(observed->Outcome(), RequestOutcome::kServerShutdown);
  complete(MakeTask());
  EXPECT_EQ(transport.reply()->count.load(), 1);
}
TEST(AsyncDispatcherTest, AdmissionIsBoundedAndShutdownRejectsNewRequests) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->pending = true;
  auto held = executor->held.get_future();
  AsyncDispatcherOptions options;
  options.max_in_flight = 1;
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport first;
  ASSERT_TRUE(first.Send(dispatcher).ok());
  ASSERT_EQ(held.wait_for(kWait), std::future_status::ready);
  FakeTransport second;
  EXPECT_FALSE(second.Send(dispatcher).ok());
  EXPECT_EQ(second.reply()->count.load(), 0);
  dispatcher.Shutdown();
  EXPECT_FALSE(first.Receive().ok());
  EXPECT_FALSE(second.Send(dispatcher).ok());
  dispatcher.Shutdown();
}
TEST(AsyncDispatcherTest, RequiredExtensionsUseSharedValidator) {
  auto executor = std::make_shared<FakeExecutor>();
  AsyncDispatcherOptions options;
  options.required_extensions = {kExtension};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport missing;
  ASSERT_TRUE(missing.Send(dispatcher).ok());
  auto failure = missing.Receive();
  ASSERT_FALSE(failure.ok());
  EXPECT_TRUE(failure.error().protocol_code().has_value());
  RequestContext metadata;
  metadata.client_headers[std::string(a2a::core::Extensions::kHeaderName)] = kExtension;
  FakeTransport activated;
  ASSERT_TRUE(activated.Send(dispatcher, GetTaskRequest(), metadata).ok());
  EXPECT_TRUE(activated.Receive().ok());
  EXPECT_EQ(executor->calls.load(), 1);
}
void ExpectStreamRejected(AsyncDispatcher& dispatcher, const DispatchRequest& request) {
  FakeTransport stream;
  ASSERT_TRUE(stream.Send(dispatcher, request).ok());
  const auto response = stream.Receive();
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().code(), a2a::core::ErrorCode::kValidation);
}

TEST(AsyncDispatcherTest, StreamingIsRejectedAndHistoryProjectionMatchesSync) {
  auto executor = std::make_shared<FakeExecutor>();
  AsyncDispatcher dispatcher(executor);
  const std::vector<DispatchRequest> streams{
      {.operation = DispatcherOperation::kSendStreamingMessage, .payload = lf::a2a::v1::SendMessageRequest{}},
      {.operation = DispatcherOperation::kSubscribeTask, .payload = lf::a2a::v1::GetTaskRequest{}},
  };
  for (const auto& request : streams) {
    ExpectStreamRejected(dispatcher, request);
  }
  auto query = GetTaskRequest();
  std::get<lf::a2a::v1::GetTaskRequest>(query.payload).set_history_length(kHistoryLimit);
  FakeTransport get;
  ASSERT_TRUE(get.Send(dispatcher, query).ok());
  auto response = get.Receive();
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(std::get<lf::a2a::v1::Task>(response.value().payload()).history_size(), kHistoryLimit);
}

class AllowPolicy final : public SecurityPolicy {
 public:
  [[nodiscard]] Result<std::string> Authenticate(const RequestContext& /*context*/) const override {
    return std::string(kPrincipal);
  }
  [[nodiscard]] bool AuthorizeOperation(const DispatchRequest& /*request*/,
                                        const RequestContext& context) const override {
    return context.principal == kPrincipal;
  }
  [[nodiscard]] bool CanAccessTask(const lf::a2a::v1::Task& task, const RequestContext& context) const override {
    return context.principal == kPrincipal && task.history_size() == kHistorySize;
  }
};
TEST(AsyncDispatcherTest, SecuritySeesFullHistoryAndAuthenticatedPrincipalBeforeProjection) {
  auto executor = std::make_shared<FakeExecutor>();
  auto interceptor = std::make_shared<RecordingInterceptor>(std::make_shared<Trace>());
  AsyncDispatcherOptions options;
  options.interceptors = {interceptor};
  options.security_policy = std::make_shared<AllowPolicy>();
  InMemoryTaskStore store;
  ASSERT_TRUE(store.CreateOrUpdate(MakeTask()).ok());
  options.task_store = &store;
  AsyncDispatcher dispatcher(executor, options);
  auto request = GetTaskRequest();
  std::get<lf::a2a::v1::GetTaskRequest>(request.payload).set_history_length(kHistoryLimit);
  FakeTransport transport;
  ASSERT_TRUE(transport.Send(dispatcher, request).ok());
  const auto result = transport.Receive();
  ASSERT_TRUE(result.ok()) << result.error().message();
  EXPECT_EQ(executor->get_history_limit, -1);
  EXPECT_EQ(executor->last_metadata.principal, kPrincipal);
  EXPECT_EQ(interceptor->after_principal, kPrincipal);
  EXPECT_EQ(std::get<lf::a2a::v1::Task>(result.value().payload()).history_size(), kHistoryLimit);
}

class BatchExecutor final : public FakeExecutor {
 public:
  explicit BatchExecutor(std::size_t expected) : started(static_cast<std::ptrdiff_t>(expected)) {}
  void GetTask(lf::a2a::v1::GetTaskRequest /*request*/, RequestExecutionContext /*context*/,
               Completion<lf::a2a::v1::Task> completion) override {
    {
      std::lock_guard lock(mutex);
      threads.insert(std::this_thread::get_id());
      callbacks.push_back(std::move(completion));
    }
    started.count_down();
  }
  std::latch started;
  std::mutex mutex;
  std::set<std::thread::id> threads;
  std::vector<Completion<lf::a2a::v1::Task>> callbacks;
};
TEST(AsyncDispatcherTest, PendingCallsDoNotOccupyWorkersAndQueueRemainsBounded) {
  auto executor = std::make_shared<BatchExecutor>(kConcurrency);
  AsyncDispatcherOptions options;
  options.worker_count = kWorkerCount;
  options.max_in_flight = kConcurrency;
  AsyncDispatcher dispatcher(executor, options);
  std::vector<std::unique_ptr<FakeTransport>> transports;
  for (std::size_t index = 0; index < kConcurrency; ++index) {
    transports.push_back(std::make_unique<FakeTransport>());
    ASSERT_TRUE(transports.back()->Send(dispatcher).ok());
  }
  executor->started.wait();
  EXPECT_LE(executor->threads.size(), kWorkerCount);
  FakeTransport overflow;
  EXPECT_FALSE(overflow.Send(dispatcher).ok());
  for (const auto& complete : executor->callbacks) {
    complete(MakeTask());
  }
  for (const auto& transport : transports) {
    EXPECT_TRUE(transport->Receive().ok());
  }
}
class BlockingInterceptor final : public ServerInterceptor {
 public:
  Result<void> BeforeDispatch(const DispatchRequest& /*request*/, RequestContext& /*context*/) override {
    started.set_value();
    release.get_future().wait();
    return {};
  }
  std::promise<void> started;
  std::promise<void> release;
};
TEST(AsyncDispatcherTest, CancelDuringBeforeWaitsForHookBeforeAfterAndSkipsExecution) {
  auto executor = std::make_shared<FakeExecutor>();
  auto blocker = std::make_shared<BlockingInterceptor>();
  auto started = blocker->started.get_future();
  auto trace = std::make_shared<Trace>();
  auto recorder = std::make_shared<RecordingInterceptor>(trace);
  AsyncDispatcherOptions options;
  options.interceptors = {blocker, recorder};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  auto accepted = transport.Send(dispatcher);
  ASSERT_TRUE(accepted.ok());
  // Release even if the expectation fails, so destruction never waits on the hook.
  EXPECT_EQ(started.wait_for(kWait), std::future_status::ready);
  EXPECT_TRUE(accepted.value()->Cancel());
  EXPECT_EQ(recorder->after_count.load(), 0);
  blocker->release.set_value();
  EXPECT_FALSE(transport.Receive().ok());
  EXPECT_EQ(executor->calls.load(), 0);
  EXPECT_EQ(recorder->after_count.load(), 1);
}
TEST(AsyncDispatcherTest, DuplicateSuccessCannotChangeFinalOutcome) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->pending = true;
  auto held = executor->held.get_future();
  AsyncDispatcher dispatcher(executor);
  FakeTransport transport;
  auto accepted = transport.Send(dispatcher);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(held.wait_for(kWait), std::future_status::ready);
  auto complete = held.get();
  complete(MakeTask());
  EXPECT_TRUE(transport.Receive().ok());
  complete(Error::Validation(kFailure));
  EXPECT_FALSE(accepted.value()->Cancel());
  EXPECT_EQ(transport.reply()->count.load(), 1);
  EXPECT_EQ(accepted.value()->Context().Outcome(), RequestOutcome::kSucceeded);
  EXPECT_EQ(transport.reply()->outcome, RequestOutcome::kSucceeded);
}
TEST(AsyncDispatcherTest, DispatcherCanBeDestroyedFromItsOwnCompletion) {
  auto executor = std::make_shared<FakeExecutor>();
  AsyncDispatcherOptions options;
  options.worker_count = 1;
  auto dispatcher = std::make_unique<AsyncDispatcher>(executor, options);
  std::promise<void> destroyed;
  auto result = destroyed.get_future();
  // Only the completion accesses dispatcher after submission.
  auto* target = dispatcher.get();
  const auto accepted = target->Dispatch(GetTaskRequest(), {}, [&](auto response, RequestExecutionContext context) {
    EXPECT_TRUE(response.ok());
    EXPECT_EQ(context.Outcome(), RequestOutcome::kSucceeded);
    dispatcher.reset();
    destroyed.set_value();
  });
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(result.wait_for(kWait), std::future_status::ready);
  EXPECT_EQ(dispatcher, nullptr);
}
TEST(AsyncDispatcherTest, ThrowingAfterHookDoesNotPreventOtherHooksOrCompletion) {
  auto executor = std::make_shared<FakeExecutor>();
  auto recorder = std::make_shared<RecordingInterceptor>(std::make_shared<Trace>());
  auto throwing = std::make_shared<RecordingInterceptor>(std::make_shared<Trace>());
  throwing->throw_after = true;
  AsyncDispatcherOptions options;
  options.interceptors = {recorder, throwing};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  ASSERT_TRUE(transport.Send(dispatcher).ok());
  EXPECT_TRUE(transport.Receive().ok());
  EXPECT_EQ(recorder->after_count.load(), 1);
  EXPECT_EQ(throwing->after_count.load(), 1);
}
TEST(AsyncDispatcherTest, InvalidConfigurationAndMissingCompletionFailImmediately) {
  AsyncDispatcherOptions options;
  options.worker_count = 0;
  EXPECT_THROW(AsyncDispatcher(std::make_shared<FakeExecutor>(), options), std::invalid_argument);
  EXPECT_THROW(AsyncDispatcher(nullptr), std::invalid_argument);
  AsyncDispatcher dispatcher(std::make_shared<FakeExecutor>());
  EXPECT_FALSE(dispatcher.Dispatch(GetTaskRequest(), {}, {}).ok());
}
TEST(AsyncDispatcherTest, SecurityListUsesSameAuthorizedStorePath) {
  auto executor = std::make_shared<FakeExecutor>();
  AsyncDispatcherOptions options;
  options.security_policy = std::make_shared<AllowPolicy>();
  InMemoryTaskStore store;
  ASSERT_TRUE(store.CreateOrUpdate(MakeTask()).ok());
  options.task_store = &store;
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  ASSERT_TRUE(
      transport.Send(dispatcher, {.operation = DispatcherOperation::kListTasks, .payload = ListTasksRequest{}}).ok());
  const auto result = transport.Receive();
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(std::get<ListTasksResponse>(result.value().payload()).tasks.size(), 1);
  EXPECT_EQ(executor->calls.load(), 0);
}
class CheckingCardProvider final : public a2a::core::AgentCardProvider {
 public:
  [[nodiscard]] Result<lf::a2a::v1::AgentCard> GetExtendedAgentCard(
      const a2a::core::AgentCardRequestContext& context) const override {
    EXPECT_EQ(context.tenant, kTenant);
    EXPECT_EQ(context.remote_address, kRequestId);
    EXPECT_EQ(context.auth_metadata.at(kRequestId), kInterceptedId);
    lf::a2a::v1::AgentCard card;
    card.set_name(kCardName);
    return card;
  }
};
TEST(AsyncDispatcherTest, ExtendedCardProviderReceivesTenantAndOwnedMetadata) {
  auto executor = std::make_shared<FakeExecutor>();
  AsyncDispatcherOptions options;
  options.agent_card_provider = std::make_shared<CheckingCardProvider>();
  AsyncDispatcher dispatcher(executor, options);
  RequestContext context;
  context.remote_address = kRequestId;
  context.auth_metadata[kRequestId] = kInterceptedId;
  lf::a2a::v1::GetExtendedAgentCardRequest request;
  request.set_tenant(kTenant);
  FakeTransport transport;
  ASSERT_TRUE(
      transport.Send(dispatcher, {.operation = DispatcherOperation::kGetExtendedAgentCard, .payload = request}, context)
          .ok());
  const auto response = transport.Receive();
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(std::get<lf::a2a::v1::AgentCard>(response.value().payload()).name(), kCardName);
  EXPECT_EQ(executor->calls.load(), 0);
}
TEST(AsyncDispatcherTest, SendMessageProjectsRequestedHistory) {
  auto executor = std::make_shared<FakeExecutor>();
  AsyncDispatcher dispatcher(executor);
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_configuration()->set_history_length(kHistoryLimit);
  FakeTransport transport;
  ASSERT_TRUE(transport.Send(dispatcher, {.operation = DispatcherOperation::kSendMessage, .payload = request}).ok());
  const auto response = transport.Receive();
  ASSERT_TRUE(response.ok());
  EXPECT_EQ(std::get<lf::a2a::v1::SendMessageResponse>(response.value().payload()).task().history_size(),
            kHistoryLimit);
}

class ThrowingExecutor final : public FakeExecutor {
 public:
  void GetTask(lf::a2a::v1::GetTaskRequest /*request*/, RequestExecutionContext /*context*/,
               Completion<lf::a2a::v1::Task> /*completion*/) override {
    throw std::runtime_error(kFailure);
  }
};
TEST(AsyncDispatcherTest, ExecutorExceptionBecomesInternalFailureAndRunsAfter) {
  auto executor = std::make_shared<ThrowingExecutor>();
  auto recorder = std::make_shared<RecordingInterceptor>(std::make_shared<Trace>());
  AsyncDispatcherOptions options;
  options.interceptors = {recorder};
  AsyncDispatcher dispatcher(executor, options);
  FakeTransport transport;
  auto accepted = transport.Send(dispatcher);
  ASSERT_TRUE(accepted.ok());
  const auto result = transport.Receive();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code(), a2a::core::ErrorCode::kInternal);
  EXPECT_EQ(accepted.value()->Context().Outcome(), RequestOutcome::kFailed);
  EXPECT_EQ(recorder->after_count.load(), 1);
}
TEST(AsyncDispatcherTest, CancellationObserverCanDestroyDispatcherBeforeSdkStopCallback) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->pending = true;
  auto held = executor->held.get_future();
  auto dispatcher = std::make_unique<AsyncDispatcher>(executor);
  FakeTransport transport;
  auto accepted = transport.Send(*dispatcher);
  ASSERT_TRUE(accepted.ok());
  ASSERT_EQ(held.wait_for(kWait), std::future_status::ready);
  std::stop_callback destroy(accepted.value()->Context().StopToken(), [&] { dispatcher.reset(); });
  EXPECT_TRUE(accepted.value()->Cancel());
  EXPECT_FALSE(transport.Receive().ok());
  EXPECT_EQ(accepted.value()->Context().Outcome(), RequestOutcome::kClientCancelled);
  EXPECT_EQ(transport.reply()->outcome, RequestOutcome::kClientCancelled);
}

TEST(AsyncDispatcherTest, TransportCancellationSchedulesObserversOffCallerThread) {
  auto executor = std::make_shared<FakeExecutor>();
  executor->pending = true;
  AsyncDispatcher dispatcher(executor);
  FakeTransport transport;
  const auto accepted = transport.Send(dispatcher);
  ASSERT_TRUE(accepted.ok());
  std::promise<std::thread::id> observed;
  auto callback_thread = observed.get_future();
  std::stop_callback observer(accepted.value()->Context().StopToken(),
                              [&] { observed.set_value(std::this_thread::get_id()); });
  dispatcher.Cancel(accepted.value());
  EXPECT_FALSE(transport.Receive().ok());
  ASSERT_EQ(callback_thread.wait_for(kWait), std::future_status::ready);
  EXPECT_NE(callback_thread.get(), std::this_thread::get_id());
}
}  // namespace
