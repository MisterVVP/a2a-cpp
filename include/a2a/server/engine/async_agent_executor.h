// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <functional>

#include "a2a/core/result.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/engine/request_execution.h"

namespace a2a::server::engine {
// Completions may be called inline or later from any thread. The dispatcher
// serializes finalization and ignores duplicate/late completions. Take ownership
// of arguments needed after the initiating method returns; never block here.
class AsyncAgentExecutor {
 public:
  template <typename T>
  using Completion = std::function<void(core::Result<T>)>;
  virtual ~AsyncAgentExecutor() = default;
  virtual void SendMessage(lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context,
                           Completion<lf::a2a::v1::SendMessageResponse> completion) = 0;
  virtual void GetTask(lf::a2a::v1::GetTaskRequest request, RequestExecutionContext context,
                       Completion<lf::a2a::v1::Task> completion) = 0;
  virtual void ListTasks(ListTasksRequest request, RequestExecutionContext context,
                         Completion<ListTasksResponse> completion) = 0;
  virtual void CancelTask(lf::a2a::v1::CancelTaskRequest request, RequestExecutionContext context,
                          Completion<lf::a2a::v1::Task> completion) = 0;
  virtual void CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request,
                                                RequestExecutionContext context,
                                                Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) = 0;
  virtual void GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                             RequestExecutionContext context,
                                             Completion<lf::a2a::v1::TaskPushNotificationConfig> completion) = 0;
  virtual void ListTaskPushNotificationConfigs(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest request, RequestExecutionContext context,
      Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion) = 0;
  virtual void DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
                                                RequestExecutionContext context, Completion<void> completion) = 0;
};
}  // namespace a2a::server::engine
