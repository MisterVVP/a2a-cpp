// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/stream_cancellation_watcher.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "a2a/core/result.h"
#include "a2a/server/server_stream_session.h"
#include "a2a/v1/a2a.pb.h"

namespace {
constexpr auto kTestPollInterval = std::chrono::seconds(1);
constexpr auto kPromptDestructionLimit = std::chrono::milliseconds(300);
constexpr auto kWaitTimeout = std::chrono::seconds(2);
constexpr auto kDestroyerBlockedProbe = std::chrono::milliseconds(50);

class WaitSignal final {
 public:
  void Signal() {
    {
      std::lock_guard lock(mutex_);
      signaled_ = true;
    }
    changed_.notify_all();
  }

  [[nodiscard]] bool Wait() const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kWaitTimeout, [this] { return signaled_; });
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  bool signaled_ = false;
};

class RecordingLiveStreamSession final : public a2a::server::ServerStreamSession {
 public:
  a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    return std::optional<lf::a2a::v1::StreamResponse>{};
  }

  [[nodiscard]] bool IsLive() const noexcept override { return live_.load(); }

  void Cancel() noexcept override {
    cancel_count_.fetch_add(1);
    live_.store(false);
    {
      std::lock_guard lock(mutex_);
      cancelled_ = true;
    }
    changed_.notify_all();
  }

  void MarkNonLive() noexcept { live_.store(false); }

  [[nodiscard]] int CancelCount() const noexcept { return cancel_count_.load(); }

  [[nodiscard]] bool WaitForCancellation() const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, kWaitTimeout, [this] { return cancelled_; });
  }

 private:
  std::atomic_bool live_ = true;
  std::atomic_int cancel_count_ = 0;
  mutable std::mutex mutex_;
  mutable std::condition_variable changed_;
  bool cancelled_ = false;
};

class BlockingCancelStreamSession final : public a2a::server::ServerStreamSession {
 public:
  a2a::core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
    return std::optional<lf::a2a::v1::StreamResponse>{};
  }

  [[nodiscard]] bool IsLive() const noexcept override { return true; }

  void Cancel() noexcept override {
    cancel_count_.fetch_add(1);
    cancel_entered_.Signal();
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return release_cancel_; });
  }

  [[nodiscard]] bool WaitForCancelEntered() const { return cancel_entered_.Wait(); }

  void ReleaseCancel() {
    {
      std::lock_guard lock(mutex_);
      release_cancel_ = true;
    }
    changed_.notify_all();
  }

  [[nodiscard]] int CancelCount() const noexcept { return cancel_count_.load(); }

 private:
  WaitSignal cancel_entered_;
  std::atomic_int cancel_count_ = 0;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool release_cancel_ = false;
};

[[nodiscard]] a2a::server::StreamCancellationWatcher::Options TestOptions(WaitSignal* before_wait) {
  return {.poll_interval = kTestPollInterval, .before_wait = [before_wait] { before_wait->Signal(); }};
}

TEST(StreamCancellationWatcherTest, PromptDestructionInterruptsTimedWait) {
  std::atomic_bool cancelled = false;
  WaitSignal before_wait;
  RecordingLiveStreamSession stream;
  auto watcher = std::make_unique<a2a::server::StreamCancellationWatcher>([&cancelled] { return cancelled.load(); },
                                                                          &stream, TestOptions(&before_wait));
  ASSERT_TRUE(before_wait.Wait());

  const auto start = std::chrono::steady_clock::now();
  watcher.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed), kPromptDestructionLimit);
  EXPECT_EQ(stream.CancelCount(), 0);
}

TEST(StreamCancellationWatcherTest, ClientCancellationCancelsStreamOnce) {
  std::atomic_bool cancelled = false;
  RecordingLiveStreamSession stream;

  {
    a2a::server::StreamCancellationWatcher watcher([&cancelled] { return cancelled.load(); }, &stream);
    cancelled.store(true);
    EXPECT_TRUE(stream.WaitForCancellation());
  }

  EXPECT_EQ(stream.CancelCount(), 1);
}

TEST(StreamCancellationWatcherTest, DestructionWaitsForInProgressCancel) {
  std::atomic_bool cancelled = true;
  BlockingCancelStreamSession stream;
  auto watcher =
      std::make_unique<a2a::server::StreamCancellationWatcher>([&cancelled] { return cancelled.load(); }, &stream);
  ASSERT_TRUE(stream.WaitForCancelEntered());

  std::promise<void> destroyed;
  auto destroyed_future = destroyed.get_future();
  std::thread destroyer([&watcher, &destroyed] {
    watcher.reset();
    destroyed.set_value();
  });

  EXPECT_EQ(destroyed_future.wait_for(kDestroyerBlockedProbe), std::future_status::timeout);
  stream.ReleaseCancel();
  EXPECT_EQ(destroyed_future.wait_for(kWaitTimeout), std::future_status::ready);
  destroyer.join();
  EXPECT_LE(stream.CancelCount(), 1);
}

TEST(StreamCancellationWatcherTest, NonLiveStreamDoesNotStartCancellationWorker) {
  std::atomic_bool cancelled = false;
  RecordingLiveStreamSession stream;
  stream.MarkNonLive();

  {
    a2a::server::StreamCancellationWatcher watcher([&cancelled] { return cancelled.load(); }, &stream);
    cancelled.store(true);
  }

  EXPECT_EQ(stream.CancelCount(), 0);
}

TEST(StreamCancellationWatcherTest, ReportsSelectedCompilePath) {
#if A2A_STREAM_CANCELLATION_WATCHER_HAS_JTHREAD
  SUCCEED() << "StreamCancellationWatcher native build uses std::jthread";
#else
  SUCCEED() << "StreamCancellationWatcher native build uses interruptible std::thread fallback";
#endif
}

}  // namespace
