// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <cstddef>
#include <memory>

#include "a2a/server/engine/async_agent_executor.h"
#include "a2a/server/engine/coroutine_agent_executor.h"

namespace a2a::server::engine {
// Runs coroutine executors through the existing dispatcher. The caller owns and
// drains the Asio execution context after dispatcher shutdown. Each operation has
// its own strand; separate operations may run concurrently. Pending coroutine
// frames retain the application executor, including after transport cancellation.
class AsioAgentExecutor final : public AsyncAgentExecutor {
 public:
  static constexpr std::size_t kDefaultMaxPending = 1024;
  AsioAgentExecutor(std::shared_ptr<CoroutineAgentExecutor> executor, boost::asio::any_io_executor scheduler,
                    std::size_t max_pending = kDefaultMaxPending);
  void SendMessage(lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context,
                   Completion<lf::a2a::v1::SendMessageResponse> completion) override;
  void GetTask(lf::a2a::v1::GetTaskRequest request, RequestExecutionContext context,
               Completion<lf::a2a::v1::Task> completion) override;
  void ListTasks(ListTasksRequest request, RequestExecutionContext context,
                 Completion<ListTasksResponse> completion) override;
  void CancelTask(lf::a2a::v1::CancelTaskRequest request, RequestExecutionContext context,
                  Completion<lf::a2a::v1::Task> completion) override;
  void CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request,
                                        RequestExecutionContext context,
                                        Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) override;
  void GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                     RequestExecutionContext context,
                                     Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) override;
  void ListTaskPushNotificationConfigs(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest request, RequestExecutionContext context,
      Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion) override;
  void DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
                                        RequestExecutionContext context, Completion<void> completion) override;

 private:
  struct State;
  std::shared_ptr<State> state_;
};
}  // namespace a2a::server::engine
