// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <chrono>

#include "a2a/client/client_types.h"
#include "a2a/core/async_operation.h"
#include "a2a/core/non_copyable.h"
#if defined(_WIN32) && defined(SendMessage)
#undef SendMessage
#endif

namespace a2a::client {
// Network implementations compose this lifecycle contract beside sync transports.
class AsyncClientTransport : private core::NonCopyableOrMovable {
 public:
  static constexpr std::chrono::milliseconds kDefaultAsyncTimeout{30000};
  virtual ~AsyncClientTransport() = default;

  // Accepted calls complete exactly once on shared completion workers.
  [[nodiscard]] core::Result<core::AsyncHandle> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, core::Completion<lf::a2a::v1::SendMessageResponse> completion,
      const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                        core::Completion<lf::a2a::v1::Task> completion,
                                                        const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> ListTasks(const ListTasksRequest& request,
                                                          core::Completion<ListTasksResponse> completion,
                                                          const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                           core::Completion<lf::a2a::v1::Task> completion,
                                                           const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request,
      core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion,
      const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, core::Completion<void> completion,
      const CallOptions& options = {});

  virtual void Shutdown() { async_scope_.Shutdown(); }

 protected:
  core::AsyncScope async_scope_;
  [[nodiscard]] virtual std::chrono::milliseconds DefaultAsyncTimeout() const { return kDefaultAsyncTimeout; }
  virtual void StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
                                const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion);
  virtual void StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                            const core::AsyncCompletion<lf::a2a::v1::Task>& completion);
  virtual void StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                              const core::AsyncCompletion<ListTasksResponse>& completion);
  virtual void StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                               const core::AsyncCompletion<lf::a2a::v1::Task>& completion);
  virtual void StartCreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion);
  virtual void StartGetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion);
  virtual void StartListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion);
  virtual void StartDeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options,
      const core::AsyncCompletion<void>& completion);
};
}  // namespace a2a::client
