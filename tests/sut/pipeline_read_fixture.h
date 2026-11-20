// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <stdexcept>

#include "a2a/core/protocol_errors.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/engine/coroutine_agent_executor.h"
#include "a2a/server/server_interceptor.h"
#include "a2a/server/tasks/in_memory_task_store.h"

namespace a2a::tests::sut {

inline constexpr auto kPipelineTaskId = "benchmark-task";
inline constexpr auto kPipelineContextId = "benchmark-context";
inline constexpr auto kPipelineStoreSetupFailed = "Could not seed pipeline task store";

inline core::Result<void> SeedPipelineTask(server::TaskStore& store) {
  lf::a2a::v1::Task task;
  task.set_id(kPipelineTaskId);
  task.set_context_id(kPipelineContextId);
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  return store.CreateOrUpdate(task);
}

class PipelinePolicy : public server::ServerInterceptor {
 public:
  core::Result<void> BeforeDispatch(const server::DispatchRequest& request,
                                    server::RequestContext& /*context*/) override {
    if (request.operation != server::DispatcherOperation::kGetTask) {
      return core::protocol_errors::UnsupportedOperation();
    }
    return {};
  }
};

class PipelineReadFixture final : public server::AgentExecutor, public server::engine::CoroutineAgentExecutor {
 public:
  PipelineReadFixture() {
    if (!SeedPipelineTask(store_).ok()) {
      throw std::runtime_error(kPipelineStoreSetupFailed);
    }
  }

  [[nodiscard]] core::Result<lf::a2a::v1::Task> ReadTask(const lf::a2a::v1::GetTaskRequest& request) const {
    return store_.Get(request.id());
  }

  core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                          server::RequestContext& /*context*/) override {
    return ReadTask(request);
  }

  boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(
      lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext /*context*/) override {
    co_return ReadTask(request);
  }

  core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(const lf::a2a::v1::SendMessageRequest& /*request*/,
                                                             server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }

  core::Result<std::unique_ptr<server::ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& /*request*/, server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }

  core::Result<server::ListTasksResponse> ListTasks(const server::ListTasksRequest& /*request*/,
                                                    server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }

  core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& /*request*/,
                                             server::RequestContext& /*context*/) override {
    return core::protocol_errors::UnsupportedOperation();
  }

 private:
  server::InMemoryTaskStore store_;
};

}  // namespace a2a::tests::sut
