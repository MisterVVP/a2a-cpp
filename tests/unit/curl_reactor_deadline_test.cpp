// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "curl_reactor_deadline.h"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>

namespace {

using a2a::http::detail::CurlReactorDeadline;
constexpr auto kNow = CurlReactorDeadline::Clock::time_point{};
constexpr long kTimerMs = 100L;
constexpr auto kTimerDuration = std::chrono::milliseconds(kTimerMs);
constexpr auto kSubMillisecond = std::chrono::microseconds(1);

TEST(CurlReactorDeadlineTest, DisarmedWaitIsInfinite) {
  CurlReactorDeadline timer;
  EXPECT_EQ(timer.WaitMilliseconds(kNow), -1);
  EXPECT_FALSE(timer.TakeExpired(kNow));
}

TEST(CurlReactorDeadlineTest, ImmediateTimerIsConsumedOnlyOnce) {
  CurlReactorDeadline timer;
  timer.Update(0, kNow);
  EXPECT_EQ(timer.WaitMilliseconds(kNow), 0);
  EXPECT_TRUE(timer.TakeExpired(kNow));
  EXPECT_FALSE(timer.TakeExpired(kNow));
  EXPECT_EQ(timer.WaitMilliseconds(kNow), -1);
}

TEST(CurlReactorDeadlineTest, RoundsWaitUpAndExpiresAtDeadline) {
  CurlReactorDeadline timer;
  timer.Update(kTimerMs, kNow);
  EXPECT_EQ(timer.WaitMilliseconds(kNow + kSubMillisecond), kTimerMs);
  EXPECT_EQ(timer.WaitMilliseconds(kNow + kTimerDuration - kSubMillisecond), 1);
  EXPECT_FALSE(timer.TakeExpired(kNow + kTimerDuration - kSubMillisecond));
  EXPECT_EQ(timer.WaitMilliseconds(kNow + kTimerDuration), 0);
  EXPECT_TRUE(timer.TakeExpired(kNow + kTimerDuration));
}

TEST(CurlReactorDeadlineTest, NegativeUpdateDisarmsPendingTimer) {
  CurlReactorDeadline timer;
  timer.Update(0, kNow);
  timer.Update(-1, kNow);
  EXPECT_EQ(timer.WaitMilliseconds(kNow), -1);
  EXPECT_FALSE(timer.TakeExpired(kNow + kTimerDuration));
}

TEST(CurlReactorDeadlineTest, RearmReplacesEarlierDeadline) {
  CurlReactorDeadline timer;
  timer.Update(0, kNow);
  timer.Update(kTimerMs, kNow);
  EXPECT_FALSE(timer.TakeExpired(kNow));
  EXPECT_TRUE(timer.TakeExpired(kNow + kTimerDuration));
}

TEST(CurlReactorDeadlineTest, BoundsLongWaitToPollAbi) {
  CurlReactorDeadline timer;
  long timeout = (std::numeric_limits<int>::max)();
  if constexpr (sizeof(long) > sizeof(int)) {
    ++timeout;
  }
  timer.Update(timeout, kNow);
  EXPECT_EQ(timer.WaitMilliseconds(kNow), (std::numeric_limits<int>::max)());
}

}  // namespace
