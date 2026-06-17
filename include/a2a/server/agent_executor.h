// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <optional>
#include <utility>

#include "a2a/core/protocol_errors.h"
#include "a2a/core/result.h"
#include "a2a/core/task_states.h"
#include "a2a/server/request_context.h"
#include "a2a/server/server_stream_session.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class AgentExecutor {
 public:
  virtual ~AgentExecutor() = default;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<std::unique_ptr<ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<std::unique_ptr<ServerStreamSession>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, RequestContext& context) {
    class CurrentTaskStreamSession final : public ServerStreamSession {
     public:
      explicit CurrentTaskStreamSession(lf::a2a::v1::Task task) { *event_.mutable_task() = std::move(task); }

      [[nodiscard]] core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() override {
        if (sent_) {
          return std::optional<lf::a2a::v1::StreamResponse>{};
        }
        sent_ = true;
        return std::optional<lf::a2a::v1::StreamResponse>{event_};
      }

     private:
      lf::a2a::v1::StreamResponse event_;
      bool sent_ = false;
    };

    auto task = GetTask(request, context);
    if (!task.ok()) {
      return task.error();
    }
    if (core::IsTerminalTaskState(task.value().status().state())) {
      return core::protocol_errors::UnsupportedOperation("task is already terminal");
    }
    return std::unique_ptr<ServerStreamSession>(std::make_unique<CurrentTaskStreamSession>(std::move(task.value())));
  }

  [[nodiscard]] virtual core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                                  RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                                   RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, RequestContext& context) {
    (void)request;
    (void)context;
    return core::protocol_errors::PushNotificationNotSupported();
  }

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, RequestContext& context) {
    (void)request;
    (void)context;
    return core::protocol_errors::PushNotificationNotSupported();
  }

  [[nodiscard]] virtual core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
                                  RequestContext& context) {
    (void)request;
    (void)context;
    return core::protocol_errors::PushNotificationNotSupported();
  }

  [[nodiscard]] virtual core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, RequestContext& context) {
    (void)request;
    (void)context;
    return core::protocol_errors::PushNotificationNotSupported();
  }
};

}  // namespace a2a::server
