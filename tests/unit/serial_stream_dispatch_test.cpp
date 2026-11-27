// SPDX-License-Identifier: Apache-2.0

#include "serial_stream_dispatch.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace {
using a2a::http::detail::kMaximumPendingDispatchBytes;
using a2a::http::detail::kMaximumPendingDispatchTasks;
using a2a::http::detail::kStreamDispatchBatchSize;
using a2a::http::detail::SerialStreamDispatch;
constexpr std::size_t kBusyStreamTasks = kStreamDispatchBatchSize * 4U;

void ExpectOrderedCallbacks(const std::vector<std::size_t>& delivered) {
  ASSERT_EQ(delivered.size(), kBusyStreamTasks);
  for (std::size_t index = 0; index < delivered.size(); ++index) {
    EXPECT_EQ(delivered[index], index);
  }
}

class SerialStreamDispatchTest : public ::testing::Test {
 protected:
  std::shared_ptr<SerialStreamDispatch> MakeStream() {
    return std::make_shared<SerialStreamDispatch>(
        [this](SerialStreamDispatch::Task task) { ready_.push_back(std::move(task)); });
  }
  void RunNext() {
    ASSERT_FALSE(ready_.empty());
    auto task = std::move(ready_.front());
    ready_.pop_front();
    task();
  }
  void Drain() {
    while (!ready_.empty()) {
      RunNext();
    }
  }
  std::deque<SerialStreamDispatch::Task> ready_;
};

TEST_F(SerialStreamDispatchTest, BusyStreamYieldsToSubscriptionWithoutReorderingCallbacks) {
  auto busy = MakeStream();
  auto subscription = MakeStream();
  std::vector<std::size_t> delivered;
  for (std::size_t index = 0; index < kBusyStreamTasks; ++index) {
    ASSERT_TRUE(busy->Enqueue([&, index] { delivered.push_back(index); }));
  }
  std::size_t tasks_before_subscription = 0;
  ASSERT_TRUE(subscription->Enqueue([&] { tasks_before_subscription = delivered.size(); }));
  RunNext();
  RunNext();
  EXPECT_EQ(tasks_before_subscription, kStreamDispatchBatchSize);
  Drain();
  ExpectOrderedCallbacks(delivered);
}

TEST_F(SerialStreamDispatchTest, EnqueueDuringCallbackKeepsOneDrainAndTerminalLast) {
  auto stream = MakeStream();
  std::size_t delivered = 0;
  bool completed = false;
  ASSERT_TRUE(stream->Enqueue([&] {
    for (std::size_t index = 0; index < kBusyStreamTasks; ++index) {
      EXPECT_TRUE(stream->Enqueue([&] {
        EXPECT_FALSE(completed);
        ++delivered;
      }));
    }
    EXPECT_TRUE(stream->Enqueue([&] { completed = true; }, 0U, true));
    EXPECT_TRUE(ready_.empty());
  }));
  RunNext();
  EXPECT_EQ(ready_.size(), 1U);
  Drain();
  EXPECT_TRUE(completed);
  EXPECT_EQ(delivered, kBusyStreamTasks);
}

TEST_F(SerialStreamDispatchTest, IdleStreamCanBeScheduledAgainAfterExactBatch) {
  auto stream = MakeStream();
  std::size_t delivered = 0;
  for (std::size_t index = 0; index < kStreamDispatchBatchSize; ++index) {
    ASSERT_TRUE(stream->Enqueue([&] { ++delivered; }));
  }
  RunNext();
  EXPECT_TRUE(ready_.empty());
  ASSERT_TRUE(stream->Enqueue([&] { ++delivered; }));
  Drain();
  EXPECT_EQ(delivered, kStreamDispatchBatchSize + 1U);
}

TEST_F(SerialStreamDispatchTest, FullTaskQueueRejectsDataButAcceptsTerminalCompletion) {
  auto stream = MakeStream();
  std::size_t delivered = 0;
  for (std::size_t index = 0; index < kMaximumPendingDispatchTasks; ++index) {
    ASSERT_TRUE(stream->Enqueue([&] { ++delivered; }));
  }
  EXPECT_FALSE(stream->Enqueue([] {}));
  bool completed = false;
  EXPECT_TRUE(stream->Enqueue([&] { completed = true; }, 0U, true));
  Drain();
  EXPECT_TRUE(completed);
  EXPECT_EQ(delivered, kMaximumPendingDispatchTasks);
}

TEST_F(SerialStreamDispatchTest, ByteBudgetIsReclaimedWhenCallbacksDrain) {
  auto stream = MakeStream();
  ASSERT_TRUE(stream->Enqueue([] {}, kMaximumPendingDispatchBytes));
  EXPECT_FALSE(stream->Enqueue([] {}, 1U));
  Drain();
  EXPECT_TRUE(stream->Enqueue([] {}, kMaximumPendingDispatchBytes));
  Drain();
}
}  // namespace
