// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <utility>

#include "a2a/client/client_types.h"
#include "a2a/core/async_operation.h"
#if defined(_WIN32) && defined(SendMessage)
#undef SendMessage
#endif
namespace a2a::client {
namespace detail {
struct ClientState;
}
class A2AClient;
// Copyable callback facade. Its owner A2AClient controls shutdown; retaining this
// view never keeps a destroyed owner open for new calls.
class AsyncClient final {
 public:
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

 private:
  friend class A2AClient;
  explicit AsyncClient(std::shared_ptr<detail::ClientState> state) : state_(std::move(state)) {}
  std::shared_ptr<detail::ClientState> state_;
};
}  // namespace a2a::client
