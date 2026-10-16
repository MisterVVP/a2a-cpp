// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

#include "a2a/server/task_subscription_service.h"
#include "a2a/server/tasks/task_lifecycle_service.h"

namespace a2a::server {

class TaskAuthorizationService final {
 public:
  using CredentialValidator = std::function<bool(const lf::a2a::v1::Task&, std::string_view)>;
  TaskAuthorizationService(TaskStore* store, CredentialValidator validator);
  [[nodiscard]] core::Result<lf::a2a::v1::Task> RequestAuthorization(const lf::a2a::v1::SendMessageRequest& request,
                                                                     const RequestContext& context,
                                                                     const lf::a2a::v1::Message& explanation);
  [[nodiscard]] core::Result<lf::a2a::v1::Task> ReceiveMessage(const lf::a2a::v1::SendMessageRequest& request);
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Authorize(std::string_view task_id, std::string_view credential);
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Complete(std::string_view task_id);
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Cancel(std::string_view task_id);
  [[nodiscard]] core::Result<std::unique_ptr<ServerStreamSession>> Subscribe(std::string_view task_id);
  [[nodiscard]] core::Result<std::unique_ptr<ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, const RequestContext& context,
      const lf::a2a::v1::Message& explanation);
  [[nodiscard]] static bool IsAuthorizationTask(const lf::a2a::v1::Task& task);
  void Shutdown();

 private:
  [[nodiscard]] core::Result<lf::a2a::v1::Task> RequestAuthorizationLocked(
      const lf::a2a::v1::SendMessageRequest& request, const RequestContext& context,
      const lf::a2a::v1::Message& explanation);
  [[nodiscard]] core::Result<lf::a2a::v1::Task> ReceiveMessageLocked(const lf::a2a::v1::SendMessageRequest& request);
  [[nodiscard]] core::Result<TaskStore::TaskSnapshot> Snapshot(std::string_view task_id) const;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Persist(TaskStore::TaskSnapshot snapshot);
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Transition(TaskStore::TaskSnapshot snapshot,
                                                           lf::a2a::v1::TaskState state);
  TaskStore* store_;
  TaskLifecycleService lifecycle_;
  CredentialValidator validator_;
  TaskSubscriptionService subscriptions_;
  std::mutex mutex_;
};

}  // namespace a2a::server
