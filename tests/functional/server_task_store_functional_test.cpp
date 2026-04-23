#include <gtest/gtest.h>

#include <future>
#include <string>
#include <vector>

#include "a2a/server/server.h"

namespace {

lf::a2a::v1::Task MakeTask(std::string id) {
  lf::a2a::v1::Task task;
  task.set_id(std::move(id));
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_SUBMITTED);
  return task;
}

TEST(ServerTaskStoreFunctionalTest, SupportsCreateGetListAndCancelLifecycle) {
  a2a::server::InMemoryTaskStore store;

  ASSERT_TRUE(store.CreateOrUpdate(MakeTask("task-1")).ok());
  ASSERT_TRUE(store.CreateOrUpdate(MakeTask("task-2")).ok());

  const auto get_result = store.Get("task-1");
  ASSERT_TRUE(get_result.ok());
  EXPECT_EQ(get_result.value().id(), "task-1");

  const auto list_result =
      store.List(a2a::server::ListTasksRequest{.page_size = 1, .page_token = ""});
  ASSERT_TRUE(list_result.ok());
  ASSERT_EQ(list_result.value().tasks.size(), 1U);
  EXPECT_EQ(list_result.value().tasks.front().id(), "task-1");
  EXPECT_EQ(list_result.value().next_page_token, "1");

  const auto page_2 = store.List(a2a::server::ListTasksRequest{.page_size = 10, .page_token = "1"});
  ASSERT_TRUE(page_2.ok());
  ASSERT_EQ(page_2.value().tasks.size(), 1U);
  EXPECT_EQ(page_2.value().tasks.front().id(), "task-2");

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

  const auto invalid_page =
      store.List(a2a::server::ListTasksRequest{.page_size = 1, .page_token = "not-a-number"});
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
