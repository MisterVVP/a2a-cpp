// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/request_execution.h"

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <latch>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>

namespace {
using a2a::server::RequestCancellationReason;
using a2a::server::RequestCompletion;
using a2a::server::RequestContext;
using a2a::server::RequestExecution;
using a2a::server::RequestExecutionContext;
using a2a::server::RequestOutcome;

constexpr auto kDeadline = RequestExecution::Clock::time_point(std::chrono::seconds(10));
constexpr auto kBeforeDeadline = kDeadline - std::chrono::seconds(1);
constexpr auto kAfterDeadline = kDeadline + std::chrono::seconds(1);
constexpr int kRaceIterations = 64;
constexpr int kRaceParticipants = 3;
constexpr auto kRequestId = "request-id";
constexpr auto kRemoteAddress = "remote-address";
constexpr auto kAuthKey = "authorization";
constexpr auto kAuthValue = "test-credential";
constexpr auto kHeaderKey = "custom-header";
constexpr auto kHeaderValue = "custom-value";
constexpr auto kProtocolVersion = "test-version";
constexpr auto kPrincipal = "test-principal";

static_assert(std::is_aggregate_v<RequestContext>);
static_assert(!std::is_copy_constructible_v<RequestExecution>);
static_assert(!std::is_copy_assignable_v<RequestExecution>);
static_assert(!std::is_move_constructible_v<RequestExecution>);
static_assert(!std::is_move_assignable_v<RequestExecution>);

TEST(RequestExecutionTest, ContextOwnsUnchangedMetadataAfterOwnerDestruction) {
  RequestContext metadata{.request_id = kRequestId,
                          .remote_address = kRemoteAddress,
                          .auth_metadata = {{kAuthKey, kAuthValue}},
                          .client_headers = {{kHeaderKey, kHeaderValue}},
                          .protocol_version = kProtocolVersion,
                          .principal = kPrincipal};
  auto execution = std::make_unique<RequestExecution>(metadata, kDeadline);
  const auto context = execution->Context();
  const std::optional<RequestExecutionContext> copy(context);
  metadata.auth_metadata.clear();
  execution.reset();

  EXPECT_EQ(copy->Metadata().request_id, kRequestId);
  EXPECT_EQ(copy->Metadata().remote_address, kRemoteAddress);
  EXPECT_EQ(copy->Metadata().auth_metadata.at(kAuthKey), kAuthValue);
  EXPECT_EQ(copy->Metadata().client_headers.at(kHeaderKey), kHeaderValue);
  EXPECT_EQ(copy->Metadata().protocol_version, kProtocolVersion);
  EXPECT_EQ(copy->Metadata().principal, kPrincipal);
  EXPECT_EQ(copy->Deadline(), kDeadline);
  EXPECT_EQ(copy->Outcome(), RequestOutcome::kServerShutdown);
  EXPECT_TRUE(copy->StopToken().stop_requested());
}

TEST(RequestExecutionTest, DefaultIsPendingWithoutDeadlineOrCancellation) {
  RequestExecution execution;
  const auto context = execution.Context();
  EXPECT_EQ(context.Outcome(), RequestOutcome::kPending);
  EXPECT_FALSE(context.Deadline());
  EXPECT_FALSE(context.StopToken().stop_requested());
  EXPECT_FALSE(execution.ExpireDeadline(kAfterDeadline));
}

TEST(RequestExecutionTest, PreCancellationNotifiesLateObserverOnce) {
  RequestExecution execution;
  const auto context = execution.Context();
  ASSERT_TRUE(execution.Cancel());
  int notifications = 0;
  const std::stop_callback observer(context.StopToken(), [&] {
    ++notifications;
    EXPECT_EQ(context.Outcome(), RequestOutcome::kClientCancelled);
  });
  EXPECT_FALSE(execution.Cancel());
  EXPECT_FALSE(execution.TryComplete(RequestCompletion::kSucceeded));
  EXPECT_EQ(notifications, 1);
}

void CheckApplicationCompletion(RequestCompletion completion, RequestOutcome expected) {
  auto execution = std::make_unique<RequestExecution>();
  const auto context = execution->Context();
  int notifications = 0;
  const std::stop_callback observer(context.StopToken(), [&] { ++notifications; });
  EXPECT_TRUE(execution->TryComplete(completion));
  EXPECT_FALSE(execution->TryComplete(completion));
  EXPECT_FALSE(execution->Cancel());
  execution.reset();
  EXPECT_EQ(context.Outcome(), expected);
  EXPECT_FALSE(context.StopToken().stop_requested());
  EXPECT_EQ(notifications, 0);
}

TEST(RequestExecutionTest, SuccessAndFailureAreTerminalWithoutCancellation) {
  CheckApplicationCompletion(RequestCompletion::kSucceeded, RequestOutcome::kSucceeded);
  CheckApplicationCompletion(RequestCompletion::kFailed, RequestOutcome::kFailed);
}

TEST(RequestExecutionTest, DeadlineDoesNotExpireEarly) {
  RequestExecution execution({}, kDeadline);
  EXPECT_FALSE(execution.ExpireDeadline(kBeforeDeadline));
  EXPECT_EQ(execution.Context().Outcome(), RequestOutcome::kPending);
}

TEST(RequestExecutionTest, DeadlineIsExplicitAndInclusive) {
  RequestExecution execution({}, kDeadline);
  const auto context = execution.Context();
  int notifications = 0;
  const std::stop_callback observer(context.StopToken(), [&] { ++notifications; });
  EXPECT_TRUE(execution.ExpireDeadline(kDeadline));
  EXPECT_FALSE(execution.ExpireDeadline(kAfterDeadline));
  EXPECT_FALSE(execution.Cancel());
  EXPECT_FALSE(execution.TryComplete(RequestCompletion::kSucceeded, kAfterDeadline));
  EXPECT_EQ(context.Outcome(), RequestOutcome::kDeadlineExceeded);
  EXPECT_EQ(notifications, 1);
}

TEST(RequestExecutionTest, ExpiredCompletionPublishesDeadlineInsteadOfSuccessOrFailure) {
  for (const auto completion : {RequestCompletion::kSucceeded, RequestCompletion::kFailed}) {
    RequestExecution execution({}, kDeadline);
    EXPECT_FALSE(execution.TryComplete(completion, kDeadline));
    EXPECT_EQ(execution.Context().Outcome(), RequestOutcome::kDeadlineExceeded);
    EXPECT_TRUE(execution.Context().StopToken().stop_requested());
  }
}

TEST(RequestExecutionTest, CompletionBeforeDeadlineCannotBeOverwritten) {
  RequestExecution execution({}, kDeadline);
  EXPECT_TRUE(execution.TryComplete(RequestCompletion::kSucceeded, kBeforeDeadline));
  EXPECT_FALSE(execution.ExpireDeadline(kAfterDeadline));
  EXPECT_EQ(execution.Context().Outcome(), RequestOutcome::kSucceeded);
}

TEST(RequestExecutionTest, TransportDeadlineAndShutdownHaveDistinctReasons) {
  RequestExecution deadline;
  RequestExecution shutdown;
  EXPECT_TRUE(deadline.Cancel(RequestCancellationReason::kDeadlineExceeded));
  EXPECT_TRUE(shutdown.Cancel(RequestCancellationReason::kServerShutdown));
  EXPECT_EQ(deadline.Context().Outcome(), RequestOutcome::kDeadlineExceeded);
  EXPECT_EQ(shutdown.Context().Outcome(), RequestOutcome::kServerShutdown);
}

void ReenterAndReleaseOwner(std::unique_ptr<RequestExecution>& execution, const RequestExecutionContext& context) {
  EXPECT_EQ(context.Outcome(), RequestOutcome::kClientCancelled);
  EXPECT_FALSE(execution->TryComplete(RequestCompletion::kSucceeded));
  EXPECT_FALSE(execution->Cancel());
  execution.reset();
}

TEST(RequestExecutionTest, CancellationObserverMayReenterAndReleaseOwner) {
  auto execution = std::make_unique<RequestExecution>();
  const auto context = execution->Context();
  int notifications = 0;
  const std::stop_callback observer(context.StopToken(), [&] {
    ++notifications;
    ReenterAndReleaseOwner(execution, context);
  });
  EXPECT_TRUE(execution->Cancel());
  EXPECT_FALSE(execution);
  EXPECT_EQ(notifications, 1);
  EXPECT_EQ(context.Outcome(), RequestOutcome::kClientCancelled);
}

TEST(RequestExecutionTest, ObserverCanReleaseLastContextAndOwnerDuringCancellation) {
  auto execution = std::make_unique<RequestExecution>();
  const auto token = execution->Context().StopToken();
  const std::stop_callback observer(token, [&] { execution.reset(); });
  EXPECT_TRUE(execution->Cancel());
  EXPECT_FALSE(execution);
  EXPECT_TRUE(token.stop_requested());
}

TEST(RequestExecutionTest, DeregisteredObserverIsNotCalled) {
  RequestExecution execution;
  int notifications = 0;
  {
    const std::stop_callback observer(execution.Context().StopToken(), [&] { ++notifications; });
  }
  EXPECT_TRUE(execution.Cancel());
  EXPECT_EQ(notifications, 0);
}

TEST(RequestExecutionTest, ObserverDestructionJoinsRunningCallback) {
  RequestExecution execution;
  std::latch callback_entered(1);
  std::latch release_callback(1);
  std::latch destruction_started(1);
  std::atomic_bool callback_finished = false;
  auto callback = [&] {
    callback_entered.count_down();
    release_callback.wait();
    callback_finished.store(true);
  };
  auto observer = std::make_unique<std::stop_callback<decltype(callback)>>(execution.Context().StopToken(), callback);
  std::thread canceller([&] { EXPECT_TRUE(execution.Cancel()); });
  callback_entered.wait();
  std::thread destroyer([&] {
    destruction_started.count_down();
    observer.reset();
    EXPECT_TRUE(callback_finished.load());
  });
  destruction_started.wait();
  release_callback.count_down();
  canceller.join();
  destroyer.join();
}

TEST(RequestExecutionTest, CancellationAndCompletionHaveExactlyOneWinner) {
  for (int iteration = 0; iteration < kRaceIterations; ++iteration) {
    RequestExecution execution;
    std::barrier start(kRaceParticipants);
    std::atomic_int notifications = 0;
    const std::stop_callback observer(execution.Context().StopToken(), [&] { ++notifications; });
    bool cancelled = false;
    bool completed = false;
    std::thread canceller([&] {
      start.arrive_and_wait();
      cancelled = execution.Cancel();
    });
    std::thread completer([&] {
      start.arrive_and_wait();
      completed = execution.TryComplete(RequestCompletion::kSucceeded);
    });
    start.arrive_and_wait();
    canceller.join();
    completer.join();
    EXPECT_NE(cancelled, completed);
    EXPECT_EQ(execution.Context().Outcome(), cancelled ? RequestOutcome::kClientCancelled : RequestOutcome::kSucceeded);
    EXPECT_EQ(notifications.load(), cancelled ? 1 : 0);
  }
}

TEST(RequestExecutionTest, ConcurrentCancellationReasonsNotifyOnlyOnce) {
  RequestExecution execution({}, kDeadline);
  const auto context = execution.Context();
  std::barrier start(kRaceParticipants);
  std::atomic_int winners = 0;
  std::atomic_int notifications = 0;
  const std::stop_callback observer(context.StopToken(), [&] { ++notifications; });
  std::thread deadline([&] {
    start.arrive_and_wait();
    winners += execution.ExpireDeadline(kDeadline) ? 1 : 0;
  });
  std::thread shutdown([&] {
    start.arrive_and_wait();
    winners += execution.Cancel(RequestCancellationReason::kServerShutdown) ? 1 : 0;
  });
  start.arrive_and_wait();
  deadline.join();
  shutdown.join();
  EXPECT_EQ(winners.load(), 1);
  EXPECT_EQ(notifications.load(), 1);
  EXPECT_TRUE(context.Outcome() == RequestOutcome::kDeadlineExceeded ||
              context.Outcome() == RequestOutcome::kServerShutdown);
}

}  // namespace
