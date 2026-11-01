// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "curl_windows_readiness.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <utility>
#include <vector>

#include "support/curl_reactor_test_support.h"

namespace {

using a2a::http::detail::CurlWindowsReadiness;
using namespace a2a::http::testing;
constexpr long kBoundedWaitMs = 5000L;
constexpr long kImmediateWaitMs = 0L;
constexpr std::size_t kBeyondWaitHandleLimit = 96U;
constexpr std::size_t kWakeBurstCount = 10000U;

void ExpectSocketInterest(CurlWindowsReadiness& backend, SOCKET socket, int action) {
  ASSERT_TRUE(backend.Update(socket, action));
}

void ExpectReadablePair(CurlWindowsReadiness& backend, const std::pair<TestSocket, TestSocket>& pair) {
  ASSERT_TRUE(backend.Update(pair.first.get(), CURL_POLL_IN));
  ASSERT_TRUE(SendTestBytes(pair.second.get(), kTestFirstChunk));
}

void ExpectReadableEvent(CurlWindowsReadiness& backend, const CurlWindowsReadiness::Event& event) {
  EXPECT_TRUE(backend.IsCurrent(event));
  EXPECT_NE(event.events & POLLRDNORM, 0);
  EXPECT_TRUE(backend.Update(event.socket, CURL_POLL_REMOVE));
}

std::size_t CollectReadableEvents(CurlWindowsReadiness& backend) {
  if (!backend.Wait() || backend.deadline().TakeExpired()) {
    throw std::runtime_error(std::string(kSocketFailure));
  }
  for (const auto& event : backend.events()) {
    ExpectReadableEvent(backend, event);
  }
  return backend.events().size();
}

CurlWindowsReadiness::Event ReadSingleEvent(CurlWindowsReadiness& backend) {
  backend.deadline().Update(kBoundedWaitMs);
  if (!backend.Wait() || backend.events().size() != 1U) {
    throw std::runtime_error(std::string(kSocketFailure));
  }
  return backend.events().front();
}

void ExpectSnapshotInvalidated(CurlWindowsReadiness& backend, const CurlWindowsReadiness::Event& event, int action) {
  ASSERT_TRUE(backend.Update(event.socket, action));
  EXPECT_FALSE(backend.IsCurrent(event));
}

TEST(CurlWindowsReadinessTest, WakeQueuedBeforeWaitIsRetainedAndDrained) {
  CurlWindowsReadiness backend;
  ASSERT_TRUE(backend.valid());
  backend.deadline().Update(kBoundedWaitMs);
  backend.Wake();
  ASSERT_TRUE(backend.Wait());
  EXPECT_FALSE(backend.deadline().TakeExpired());
  EXPECT_TRUE(backend.DrainWake());
  EXPECT_TRUE(backend.events().empty());
}

TEST(CurlWindowsReadinessTest, WakeInterruptsAnIdleWait) {
  CurlWindowsReadiness backend;
  ASSERT_TRUE(backend.valid());
  backend.deadline().Update(kBoundedWaitMs);
  std::promise<void> entering;
  auto entered = entering.get_future();
  auto waited = std::async(std::launch::async, [&] {
    entering.set_value();
    return backend.Wait();
  });
  entered.wait();
  backend.Wake();
  ASSERT_EQ(waited.wait_for(kReactorTestTimeout), std::future_status::ready);
  EXPECT_TRUE(waited.get());
  EXPECT_FALSE(backend.deadline().TakeExpired());
  EXPECT_TRUE(backend.DrainWake());
}

TEST(CurlWindowsReadinessTest, ManyWakeCommandsDoNotBlockProducers) {
  CurlWindowsReadiness backend;
  ASSERT_TRUE(backend.valid());
  for (std::size_t index = 0; index < kWakeBurstCount; ++index) {
    backend.Wake();
  }
  backend.deadline().Update(kBoundedWaitMs);
  ASSERT_TRUE(backend.Wait());
  EXPECT_TRUE(backend.DrainWake());
}

TEST(CurlWindowsReadinessTest, ImmediateAndPositiveTimersExpireWithoutSockets) {
  CurlWindowsReadiness backend;
  ASSERT_TRUE(backend.valid());
  constexpr long kShortTimerMs = 10L;
  for (const long timeout : {kImmediateWaitMs, kShortTimerMs}) {
    backend.deadline().Update(timeout);
    ASSERT_TRUE(backend.Wait());
    EXPECT_TRUE(backend.deadline().TakeExpired());
    EXPECT_TRUE(backend.events().empty());
  }
}

TEST(CurlWindowsReadinessTest, PollsMoreThanSixtyFourSocketsWithoutHandleCeiling) {
  CurlWindowsReadiness backend;
  ASSERT_TRUE(backend.valid());
  std::vector<std::pair<TestSocket, TestSocket>> pairs;
  pairs.reserve(kBeyondWaitHandleLimit);
  for (std::size_t index = 0; index < kBeyondWaitHandleLimit; ++index) {
    pairs.push_back(MakeTestSocketPair());
    ExpectReadablePair(backend, pairs.back());
  }
  backend.deadline().Update(kBoundedWaitMs);
  std::size_t ready_count = 0;
  while (ready_count < kBeyondWaitHandleLimit) {
    ready_count += CollectReadableEvents(backend);
  }
  EXPECT_EQ(ready_count, kBeyondWaitHandleLimit);
}

TEST(CurlWindowsReadinessTest, RemovedReusedAndChangedRegistrationsInvalidateSnapshots) {
  CurlWindowsReadiness backend;
  ASSERT_TRUE(backend.valid());
  auto [reader, writer] = MakeTestSocketPair();
  ASSERT_TRUE(backend.Update(reader.get(), CURL_POLL_IN));
  ASSERT_TRUE(SendTestBytes(writer.get(), kTestFirstChunk));
  const auto snapshot = ReadSingleEvent(backend);
  ASSERT_TRUE(backend.IsCurrent(snapshot));
  ExpectSnapshotInvalidated(backend, snapshot, CURL_POLL_NONE);
  ASSERT_TRUE(backend.Update(reader.get(), CURL_POLL_IN));
  EXPECT_FALSE(backend.IsCurrent(snapshot));
  const auto updated = ReadSingleEvent(backend);
  ExpectSnapshotInvalidated(backend, updated, CURL_POLL_INOUT);
}

TEST(CurlWindowsReadinessTest, RemovingMiddleRegistrationPreservesMovedSocket) {
  CurlWindowsReadiness backend;
  ASSERT_TRUE(backend.valid());
  auto first = MakeTestSocketPair();
  auto second = MakeTestSocketPair();
  ExpectSocketInterest(backend, first.first.get(), CURL_POLL_IN);
  ExpectSocketInterest(backend, second.first.get(), CURL_POLL_OUT);
  ExpectSocketInterest(backend, first.first.get(), CURL_POLL_REMOVE);
  ExpectSocketInterest(backend, second.first.get(), CURL_POLL_IN);
  ASSERT_TRUE(SendTestBytes(second.second.get(), kTestFirstChunk));
  const auto event = ReadSingleEvent(backend);
  EXPECT_EQ(event.socket, second.first.get());
  EXPECT_TRUE(backend.IsCurrent(event));
}

TEST(CurlWindowsReadinessTest, PeerClosureIsReported) {
  CurlWindowsReadiness backend;
  ASSERT_TRUE(backend.valid());
  auto [reader, writer] = MakeTestSocketPair();
  ASSERT_TRUE(backend.Update(reader.get(), CURL_POLL_IN));
  writer.Close();
  backend.deadline().Update(kBoundedWaitMs);
  ASSERT_TRUE(backend.Wait());
  ASSERT_EQ(backend.events().size(), 1U);
  EXPECT_NE(backend.events().front().events & (POLLRDNORM | POLLHUP | POLLERR), 0);
}

}  // namespace
