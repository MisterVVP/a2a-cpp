// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)
#include "client/stream_worker_executor.h"

#include <gtest/gtest.h>

#include <barrier>
#include <condition_variable>
#include <cstddef>
#include <latch>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

namespace a2a::client::internal {
namespace {

constexpr std::size_t kOneIdleWorker = 1;
constexpr std::ptrdiff_t kThreeParticipants = 3;
constexpr std::ptrdiff_t kTwoTasks = 2;

TEST(StreamWorkerExecutorTest, ReusesWorkerForSequentialTasks) {
  StreamWorkerExecutor executor(kOneIdleWorker);
  std::latch first_done(1);
  std::latch second_done(1);
  std::thread::id first_id;
  std::thread::id second_id;

  executor.Submit([&] { first_id = std::this_thread::get_id(); }, [&] { first_done.count_down(); });
  first_done.wait();
  executor.Submit([&] { second_id = std::this_thread::get_id(); }, [&] { second_done.count_down(); });
  second_done.wait();

  EXPECT_EQ(first_id, second_id);
}

TEST(StreamWorkerExecutorTest, StartsAnotherTaskWhileWorkerIsBusy) {
  StreamWorkerExecutor executor(kOneIdleWorker);
  std::latch first_started(1);
  std::latch release_first(1);
  std::latch second_started(1);

  executor.Submit([&] {
    first_started.count_down();
    release_first.wait();
  });
  first_started.wait();
  executor.Submit([&] { second_started.count_down(); });
  second_started.wait();
  release_first.count_down();
}

TEST(StreamWorkerExecutorTest, RetainsNoMoreThanConfiguredIdleBound) {
  StreamWorkerExecutor executor(kOneIdleWorker);
  std::barrier tasks_started(kThreeParticipants);
  std::latch release_tasks(1);
  std::latch tasks_done(kThreeParticipants);
  std::mutex ids_mutex;
  std::set<std::thread::id> first_wave_ids;

  for (std::ptrdiff_t index = 0; index < kThreeParticipants; ++index) {
    executor.Submit([&] {
      {
        std::lock_guard lock(ids_mutex);
        first_wave_ids.insert(std::this_thread::get_id());
      }
      tasks_started.arrive_and_wait();
      release_tasks.wait();
      tasks_done.count_down();
    });
  }
  release_tasks.count_down();
  tasks_done.wait();

  std::barrier second_wave_started(kTwoTasks);
  std::latch release_second_wave(1);
  std::latch second_wave_done(kTwoTasks);
  std::set<std::thread::id> reused_ids;
  for (std::ptrdiff_t index = 0; index < kTwoTasks; ++index) {
    executor.Submit([&] {
      {
        std::lock_guard lock(ids_mutex);
        if (first_wave_ids.contains(std::this_thread::get_id())) {
          reused_ids.insert(std::this_thread::get_id());
        }
      }
      second_wave_started.arrive_and_wait();
      release_second_wave.wait();
      second_wave_done.count_down();
    });
  }
  release_second_wave.count_down();
  second_wave_done.wait();

  EXPECT_LE(reused_ids.size(), kOneIdleWorker);
}

TEST(StreamWorkerExecutorTest, ShutdownWaitsForActiveTaskAndStopsIdleWorkers) {
  std::latch task_started(1);
  std::latch release_task(1);
  std::latch task_done(1);
  auto executor = std::make_unique<StreamWorkerExecutor>(kOneIdleWorker);
  executor->Submit([&] {
    task_started.count_down();
    release_task.wait();
    task_done.count_down();
  });
  task_started.wait();
  release_task.count_down();
  task_done.wait();
  executor.reset();
  SUCCEED();
}

}  // namespace
}  // namespace a2a::client::internal
