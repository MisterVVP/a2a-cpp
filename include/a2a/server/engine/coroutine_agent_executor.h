// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <boost/asio/awaitable.hpp>
#include <utility>

#include "a2a/core/result.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/engine/request_execution.h"

namespace a2a::server::engine {
// Arguments are owned by the coroutine frame. Operations must suspend for I/O,
// propagate Asio cancellation, and never block the event-loop threads.
class CoroutineAgentExecutor {
 public:
  virtual ~CoroutineAgentExecutor() = default;
  virtual boost::asio::awaitable<core::Result<lf::a2a::v1::SendMessageResponse>> SendMessage(
      lf::a2a::v1::SendMessageRequest request, RequestExecutionContext context);
  virtual boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> GetTask(lf::a2a::v1::GetTaskRequest request,
                                                                          RequestExecutionContext context);
  virtual boost::asio::awaitable<core::Result<ListTasksResponse>> ListTasks(ListTasksRequest request,
                                                                            RequestExecutionContext context);
  virtual boost::asio::awaitable<core::Result<lf::a2a::v1::Task>> CancelTask(lf::a2a::v1::CancelTaskRequest request,
                                                                             RequestExecutionContext context);
  virtual boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>>
  CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request, RequestExecutionContext context);
  virtual boost::asio::awaitable<core::Result<lf::a2a::v1::TaskPushNotificationConfig>> GetTaskPushNotificationConfig(
      lf::a2a::v1::GetTaskPushNotificationConfigRequest request, RequestExecutionContext context);
  virtual boost::asio::awaitable<core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>>
  ListTaskPushNotificationConfigs(lf::a2a::v1::ListTaskPushNotificationConfigsRequest request,
                                  RequestExecutionContext context);
  virtual boost::asio::awaitable<core::Result<void>> DeleteTaskPushNotificationConfig(
      lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request, RequestExecutionContext context);
};
}  // namespace a2a::server::engine
