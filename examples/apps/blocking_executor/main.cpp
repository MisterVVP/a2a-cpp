// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <utility>

#include "a2a/core/protocol_errors.h"
#include "a2a/server/engine/async_runtime.h"
#include "a2a/server/engine/blocking_executor_adapter.h"

namespace {
using namespace a2a;
using namespace a2a::server;
using namespace a2a::server::engine;
constexpr auto kTaskId = "blocking-example-task";
constexpr auto kTimeout = std::chrono::seconds(5);
constexpr std::size_t kBackendWorkers = 2;
constexpr std::size_t kBackendLimit = 64;

// Stand-in for an existing thread-safe synchronous application. Real blocking
// backends keep their AgentExecutor methods and explicitly opt into adaptation.
class LegacyExecutor final : public AgentExecutor {
 public:
  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          RequestContext& /*context*/) override {
    lf::a2a::v1::Task task;
    task.set_id(request.id());
    return task;
  }
  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& /*request*/,
                                                             RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<std::unique_ptr<ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& /*request*/, RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& /*request*/, RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& /*request*/,
                                             RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }
};
}  // namespace

int main() {
  auto legacy = std::make_shared<LegacyExecutor>();
  auto backend = std::make_shared<BlockingExecutorAdapter>(
      legacy, BlockingExecutorOptions{.workers = kBackendWorkers, .max_pending = kBackendLimit});
  AsyncRuntime runtime(backend);
  lf::a2a::v1::GetTaskRequest request;
  request.set_id(kTaskId);
  std::promise<bool> completion;
  auto reply = completion.get_future();
  const auto admitted = runtime.dispatcher()->Dispatch(
      {.operation = DispatcherOperation::kGetTask, .payload = std::move(request)}, {},
      [&completion](auto result, auto) {
        completion.set_value(result.ok() && std::get<lf::a2a::v1::Task>(result.value().payload()).id() == kTaskId);
      });
  if (!admitted.ok()) {
    return EXIT_FAILURE;
  }
  const bool ready = reply.wait_for(kTimeout) == std::future_status::ready;
  runtime.Join();
  return ready && reply.get() ? EXIT_SUCCESS : EXIT_FAILURE;
}
