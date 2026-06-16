// SPDX-License-Identifier: Apache-2.0
#include "a2a/server/task_id_generator.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "a2a/server/request_context.h"
#include "a2a/server/tasks/in_memory_task_store.h"
#include "a2a/server/tasks/task_lifecycle_service.h"
#include "a2a/v1/a2a.pb.h"

namespace {

constexpr std::string_view kTaskPrefix = "task-";
constexpr std::string_view kFirstMessageId = "m-1";
constexpr std::string_view kUuidPatternText = R"(^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$)";
constexpr std::size_t kTaskPrefixSize = 5;
constexpr std::size_t kUuidVersionOffset = 14;
constexpr std::size_t kUuidVariantOffset = 19;
constexpr std::size_t kManyGeneratedIdCount = 2048;
constexpr std::size_t kThreadCount = 4;
constexpr std::size_t kIdsPerThread = 512;
constexpr std::size_t kMaxThreadGenerationAttempts = kIdsPerThread * 8;
constexpr std::chrono::milliseconds kOverflowRetryDelay{1};
constexpr char kUuidV7VersionNibble = '7';
constexpr std::string_view kRfcCompatibleVariantNibbles = "89ab";

[[nodiscard]] bool HasTaskPrefix(const std::string& task_id) { return task_id.starts_with(kTaskPrefix); }

[[nodiscard]] std::string UuidBody(const std::string& task_id) { return task_id.substr(kTaskPrefixSize); }

[[nodiscard]] bool HasRfcCompatibleVariant(char nibble) {
  return kRfcCompatibleVariantNibbles.find(nibble) != std::string_view::npos;
}

void ExpectUuidV7TaskIdShape(const std::string& task_id) {
  EXPECT_TRUE(HasTaskPrefix(task_id));

  const std::string uuid = UuidBody(task_id);
  const std::regex uuid_pattern{std::string(kUuidPatternText)};
  EXPECT_TRUE(std::regex_match(uuid, uuid_pattern));
  EXPECT_EQ(uuid[kUuidVersionOffset], kUuidV7VersionNibble);
  EXPECT_TRUE(HasRfcCompatibleVariant(uuid[kUuidVariantOffset]));
}

[[nodiscard]] lf::a2a::v1::SendMessageRequest MakeRequest() {
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_message_id(std::string(kFirstMessageId));
  return request;
}

void GenerateThreadTaskIds(a2a::server::UuidV7TaskIdGenerator* generator,
                           const lf::a2a::v1::SendMessageRequest* request, const a2a::server::RequestContext* context,
                           std::vector<std::string>* ids, std::atomic_bool* failed_generation) {
  ids->reserve(kIdsPerThread);
  std::size_t attempts = 0;
  while (ids->size() < kIdsPerThread && attempts < kMaxThreadGenerationAttempts) {
    ++attempts;
    const auto result = generator->GenerateTaskId(*request, *context);
    if (!result.ok()) {
      std::this_thread::sleep_for(kOverflowRetryDelay);
      continue;
    }
    ids->push_back(result.value());
  }
  if (ids->size() != kIdsPerThread) {
    *failed_generation = true;
  }
}

void StartGeneratorThreads(a2a::server::UuidV7TaskIdGenerator* generator,
                           const lf::a2a::v1::SendMessageRequest* request, const a2a::server::RequestContext* context,
                           std::vector<std::vector<std::string>>* thread_ids, std::vector<std::thread>* threads,
                           std::atomic_bool* failed_generation) {
  threads->reserve(kThreadCount);
  for (std::size_t thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    threads->emplace_back(GenerateThreadTaskIds, generator, request, context, &(*thread_ids)[thread_index],
                          failed_generation);
  }
}

void JoinGeneratorThreads(std::vector<std::thread>* threads) {
  for (auto& thread : *threads) {
    thread.join();
  }
}

void ExpectThreadIdsUniqueAndValid(const std::vector<std::vector<std::string>>& thread_ids) {
  std::unordered_set<std::string> generated_ids;
  generated_ids.reserve(kThreadCount * kIdsPerThread);
  for (const auto& ids : thread_ids) {
    ASSERT_EQ(ids.size(), kIdsPerThread);
    for (const auto& task_id : ids) {
      ExpectUuidV7TaskIdShape(task_id);
      EXPECT_TRUE(generated_ids.insert(task_id).second);
    }
  }
}

class FailingTaskIdGenerator final : public a2a::server::TaskIdGenerator {
 public:
  [[nodiscard]] a2a::core::Result<std::string> GenerateTaskId(const lf::a2a::v1::SendMessageRequest& request,
                                                              const a2a::server::RequestContext& context) override {
    (void)request;
    (void)context;
    return a2a::core::Error::Internal("boom");
  }
};

TEST(TaskIdGeneratorTest, UuidV7GeneratorProducesPrefixedUuidV7AndUniqueValues) {
  a2a::server::UuidV7TaskIdGenerator generator;
  const lf::a2a::v1::SendMessageRequest request = MakeRequest();
  a2a::server::RequestContext context;

  const auto first = generator.GenerateTaskId(request, context);
  const auto second = generator.GenerateTaskId(request, context);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_NE(first.value(), second.value());
  ExpectUuidV7TaskIdShape(first.value());
  EXPECT_LT(first.value(), second.value());
}

TEST(TaskIdGeneratorTest, UuidV7GeneratorProducesManyUniqueMonotonicValues) {
  a2a::server::UuidV7TaskIdGenerator generator;
  const lf::a2a::v1::SendMessageRequest request = MakeRequest();
  a2a::server::RequestContext context;
  std::unordered_set<std::string> generated_ids;
  generated_ids.reserve(kManyGeneratedIdCount);

  std::string previous_id;
  for (std::size_t id_index = 0; id_index < kManyGeneratedIdCount; ++id_index) {
    const auto result = generator.GenerateTaskId(request, context);
    ASSERT_TRUE(result.ok());
    ExpectUuidV7TaskIdShape(result.value());
    EXPECT_TRUE(generated_ids.insert(result.value()).second);
    if (!previous_id.empty()) {
      EXPECT_LT(previous_id, result.value());
    }
    previous_id = result.value();
  }
}

TEST(TaskIdGeneratorTest, UuidV7GeneratorProducesUniqueValuesAcrossThreads) {
  a2a::server::UuidV7TaskIdGenerator generator;
  const lf::a2a::v1::SendMessageRequest request = MakeRequest();
  a2a::server::RequestContext context;
  std::atomic_bool failed_generation = false;
  std::vector<std::vector<std::string>> thread_ids(kThreadCount);
  std::vector<std::thread> threads;

  StartGeneratorThreads(&generator, &request, &context, &thread_ids, &threads, &failed_generation);
  JoinGeneratorThreads(&threads);

  ASSERT_FALSE(failed_generation);
  ExpectThreadIdsUniqueAndValid(thread_ids);
}

TEST(TaskIdGeneratorTest, LifecycleResolveTaskIdValidatesAndPropagatesErrors) {
  a2a::server::InMemoryTaskStore store;
  auto failing = std::make_shared<FailingTaskIdGenerator>();
  a2a::server::TaskLifecycleService lifecycle(&store, failing);
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest no_message_id;
  no_message_id.mutable_message()->add_parts()->set_text("x");
  EXPECT_FALSE(lifecycle.ResolveTaskIdForSendRequest(no_message_id, context).ok());

  lf::a2a::v1::SendMessageRequest generate;
  generate.mutable_message()->set_message_id("id");
  generate.mutable_message()->add_parts()->set_text("x");
  EXPECT_FALSE(lifecycle.ResolveTaskIdForSendRequest(generate, context).ok());
}

TEST(TaskIdGeneratorTest, LifecycleResolveTaskIdPreservesExplicitTaskId) {
  a2a::server::InMemoryTaskStore store;
  lf::a2a::v1::Task task;
  task.set_id("task-existing");
  task.set_context_id("ctx-1");
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  ASSERT_TRUE(store.CreateOrUpdate(task).ok());
  a2a::server::TaskLifecycleService lifecycle(&store);
  a2a::server::RequestContext context;
  lf::a2a::v1::SendMessageRequest request;
  request.mutable_message()->set_task_id("task-existing");
  request.mutable_message()->set_context_id("ctx-1");
  request.mutable_message()->set_message_id("ignored");
  const auto result = lifecycle.ResolveTaskIdForSendRequest(request, context);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), "task-existing");
}

TEST(TaskIdGeneratorTest, LifecycleResolveTaskIdRejectsContextMismatchAndTerminalTask) {
  a2a::server::InMemoryTaskStore store;
  lf::a2a::v1::Task working;
  working.set_id("task-context");
  working.set_context_id("ctx-expected");
  working.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_WORKING);
  ASSERT_TRUE(store.CreateOrUpdate(working).ok());
  lf::a2a::v1::Task terminal;
  terminal.set_id("task-terminal");
  terminal.set_context_id("ctx-t");
  terminal.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  ASSERT_TRUE(store.CreateOrUpdate(terminal).ok());

  a2a::server::TaskLifecycleService lifecycle(&store);
  a2a::server::RequestContext context;

  lf::a2a::v1::SendMessageRequest mismatch;
  mismatch.mutable_message()->set_task_id("task-context");
  mismatch.mutable_message()->set_context_id("ctx-wrong");
  mismatch.mutable_message()->set_message_id("m-1");
  EXPECT_FALSE(lifecycle.ResolveTaskIdForSendRequest(mismatch, context).ok());

  lf::a2a::v1::SendMessageRequest follow_up;
  follow_up.mutable_message()->set_task_id("task-terminal");
  follow_up.mutable_message()->set_context_id("ctx-t");
  follow_up.mutable_message()->set_message_id("m-2");
  EXPECT_FALSE(lifecycle.ResolveTaskIdForSendRequest(follow_up, context).ok());
}

}  // namespace
