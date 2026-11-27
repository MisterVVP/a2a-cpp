// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <memory>

#include "a2a/core/non_copyable.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/engine/coroutine_agent_executor.h"

// Windows headers can define SendMessage as an ANSI/Unicode macro alias.
// Keep the protocol method name stable regardless of dependency include order.
#ifdef SendMessage
#undef SendMessage
#endif

namespace a2a::server::engine {
struct BlockingExecutorOptions final {
  static constexpr std::size_t kDefaultWorkers = 2;
  static constexpr std::size_t kDefaultMaxPending = 1024;
  std::size_t workers = kDefaultWorkers;
  // Hard limit on all queued and running backend operations, including requests
  // whose transport has already completed cancellation.
  std::size_t max_pending = kDefaultMaxPending;
};

// Explicit compatibility facility for a thread-safe, blocking AgentExecutor.
// This is bounded worker adaptation, not asynchronous network or database I/O.
// Requests and the application are owned until physical backend cleanup finishes.
// Shutdown closes admission without waiting. Join/destruction drain accepted work;
// call them on the control thread, never from this adapter's backend operations.
class BlockingExecutorAdapter final : public CoroutineAgentExecutor, private core::NonCopyableOrMovable {
 public:
  explicit BlockingExecutorAdapter(std::shared_ptr<AgentExecutor> executor, BlockingExecutorOptions options = {});
  ~BlockingExecutorAdapter() override;
  void Shutdown();
  void Join();

  boost::asio::awaitable<core::Result<lf::a2a::v1::SendMessageResponse>> SendMessage(
      lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(lf::a2a::v1::GetTaskRequest request,
                                                                  RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<ListTasksResponse>> ListTasks(ListTasksRequest request,
                                                                    RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> CancelTask(lf::a2a::v1::CancelTaskRequest request,
                                                                     RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> CreateTaskPushNotificationConfig(
      lf::a2a::v1::TaskPushNotificationConfig request, RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> GetTaskPushNotificationConfig(
      lf::a2a::v1::GetTaskPushNotificationConfigRequest request, RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
  ListTaskPushNotificationConfigs(lf::a2a::v1::ListTaskPushNotificationConfigsRequest request,
                                  RequestExecutionContext context) override;
  boost::asio::awaitable<core::Result<void>> DeleteTaskPushNotificationConfig(
      lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request, RequestExecutionContext context) override;

 private:
  struct State;
  std::shared_ptr<State> state_;
  template <typename Request, typename Response>
  static boost::asio::awaitable<core::Result<Response>> Submit(
      std::shared_ptr<State> state, Request request, RequestExecutionContext context,
      core::Result<Response> (AgentExecutor::*operation)(const Request&, RequestContext&));
};
}  // namespace a2a::server::engine
