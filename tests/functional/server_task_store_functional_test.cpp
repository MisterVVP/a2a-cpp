// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <gtest/gtest.h>

#include <cstdint>
#include <future>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"

namespace {

constexpr char kBinaryBoundaryTaskId[] = "task-z\0boundary";
constexpr std::string_view kFollowingTaskId = "task-a";
constexpr std::int64_t kCursorTestTime = 100;

lf::a2a::v1::Task MakeTask(std::string id) {
  lf::a2a::v1::Task task;
  task.set_id(std::move(id));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_SUBMITTED);
  return task;
}

void ExpectSingleListedTask(const a2a::server::ListTasksResponse& page, std::string_view expected) {
  ASSERT_EQ(page.tasks.size(), 1U);
  EXPECT_EQ(page.tasks.front().id(), expected);
}

void StartBinaryCursor(a2a::server::InMemoryTaskStore* store, a2a::server::ListTasksRequest* query) {
  ASSERT_TRUE(store->CreateOrUpdate(MakeTask(std::string(kFollowingTaskId))).ok());
  ASSERT_TRUE(
      store->CreateOrUpdate(MakeTask(std::string(kBinaryBoundaryTaskId, sizeof(kBinaryBoundaryTaskId) - 1U))).ok());
  const auto first = store->List(*query);
  ASSERT_TRUE(first.ok());
  ExpectSingleListedTask(first.value(), std::string_view(kBinaryBoundaryTaskId, sizeof(kBinaryBoundaryTaskId) - 1U));
  ASSERT_FALSE(first.value().next_page_token.empty());
  query->page_token = first.value().next_page_token;
}

TEST(ServerTaskStoreFunctionalTest, ContinuesAfterTaskIdContainingNul) {
  a2a::server::InMemoryTaskStore store(nullptr, [] { return kCursorTestTime; });
  a2a::server::ListTasksRequest query{1, {}};
  ASSERT_NO_FATAL_FAILURE(StartBinaryCursor(&store, &query));
  const auto next = store.List(query);
  ASSERT_TRUE(next.ok());
  ExpectSingleListedTask(next.value(), kFollowingTaskId);
  EXPECT_TRUE(next.value().next_page_token.empty());
}

TEST(ServerTaskStoreFunctionalTest, SupportsCreateGetListAndCancelLifecycle) {
  a2a::server::InMemoryTaskStore store;

  ASSERT_TRUE(store.CreateOrUpdate(MakeTask("task-1")).ok());
  ASSERT_TRUE(store.CreateOrUpdate(MakeTask("task-2")).ok());

  const auto get_result = store.Get("task-1");
  ASSERT_TRUE(get_result.ok());
  EXPECT_EQ(get_result.value().id(), "task-1");

  const auto list_result = store.List(a2a::server::ListTasksRequest{1, ""});
  ASSERT_TRUE(list_result.ok());
  ASSERT_EQ(list_result.value().tasks.size(), 1U);
  EXPECT_EQ(list_result.value().tasks.front().id(), "task-2");
  EXPECT_FALSE(list_result.value().next_page_token.empty());

  const auto page_2 = store.List(a2a::server::ListTasksRequest{10, list_result.value().next_page_token});
  ASSERT_TRUE(page_2.ok());
  ASSERT_EQ(page_2.value().tasks.size(), 1U);
  EXPECT_EQ(page_2.value().tasks.front().id(), "task-1");

  const auto cancel_result = store.Cancel("task-2");
  ASSERT_TRUE(cancel_result.ok());
  EXPECT_EQ(cancel_result.value().status().state(), lf::a2a::v1::TASK_STATE_CANCELED);
}

TEST(ServerTaskStoreFunctionalTest, ValidatesInputs) {
  a2a::server::InMemoryTaskStore store;

  lf::a2a::v1::Task missing_id_task;
  const auto create_result = store.CreateOrUpdate(missing_id_task);
  ASSERT_FALSE(create_result.ok());

  const auto missing_get = store.Get("unknown");
  ASSERT_FALSE(missing_get.ok());
  EXPECT_EQ(missing_get.error().protocol_code().value_or(""), "-32001");

  const auto missing_cancel = store.Cancel("unknown");
  ASSERT_FALSE(missing_cancel.ok());
  EXPECT_EQ(missing_cancel.error().protocol_code().value_or(""), "-32001");

  const auto invalid_page = store.List(a2a::server::ListTasksRequest{1, "not-a-number"});
  ASSERT_FALSE(invalid_page.ok());
}

TEST(ServerTaskStoreFunctionalTest, HandlesConcurrentWritesAndReads) {
  a2a::server::InMemoryTaskStore store;
  constexpr int kTaskCount = 40;

  std::vector<std::future<void>> workers;
  workers.reserve(kTaskCount);
  for (int index = 0; index < kTaskCount; ++index) {
    workers.push_back(std::async(std::launch::async, [&store, index]() {
      auto task = MakeTask("task-" + std::to_string(index));
      const auto result = store.CreateOrUpdate(task);
      ASSERT_TRUE(result.ok());
    }));
  }

  for (auto& worker : workers) {
    worker.get();
  }

  const auto list_result = store.List(a2a::server::ListTasksRequest{});
  ASSERT_TRUE(list_result.ok());
  EXPECT_EQ(static_cast<int>(list_result.value().tasks.size()), kTaskCount);
}

}  // namespace
