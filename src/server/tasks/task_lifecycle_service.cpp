// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/task_lifecycle_service.h"

#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif
#include "a2a/core/task_states.h"

namespace a2a::server {
namespace {

constexpr std::string_view kTaskSnapshotIdMismatchMessage = "task snapshot does not match message.taskId";

}  // namespace

TaskLifecycleService::TaskLifecycleService(TaskStore* store, std::shared_ptr<TaskIdGenerator> task_id_generator)
    : store_(store), task_id_generator_(std::move(task_id_generator)) {
  if (task_id_generator_ == nullptr) {
    task_id_generator_ = std::make_shared<UuidV7TaskIdGenerator>();
  }
}

core::Result<lf::a2a::v1::Task> TaskLifecycleService::CreateOrUpdateTask(const lf::a2a::v1::Task& task) const {
  if (store_ == nullptr) {
    return core::Error::Internal("TaskLifecycleService store is not configured");
  }
  const auto upsert = store_->CreateOrUpdate(task);
  if (!upsert.ok()) {
    return upsert.error();
  }
  return store_->Get(task.id());
}

core::Result<std::string> TaskLifecycleService::ResolveTaskIdForSendRequest(
    const lf::a2a::v1::SendMessageRequest& request, const RequestContext& context) const {
  if (!request.has_message()) {
    return core::Error::Validation("message is required");
  }
  const auto& message = request.message();
  if (!message.task_id().empty()) {
    return message.task_id();
  }
  if (message.message_id().empty()) {
    return core::Error::Validation("message.messageId is required when message.taskId is absent");
  }
  return task_id_generator_->GenerateTaskId(request, context);
}

core::Result<void> TaskLifecycleService::ValidateTaskForSendRequest(const lf::a2a::v1::SendMessageRequest& request,
                                                                    const lf::a2a::v1::Task& task) {
  if (!request.has_message()) {
    return core::Error::Validation("message is required");
  }
  const auto& message = request.message();
  if (!message.task_id().empty() && task.id() != message.task_id()) {
    return core::Error::Validation(std::string(kTaskSnapshotIdMismatchMessage));
  }
  if (!message.context_id().empty() && !task.context_id().empty() && message.context_id() != task.context_id()) {
    return core::protocol_errors::UnsupportedOperation("contextId does not match task");
  }
  if (core::IsTerminalTaskState(task.status().state())) {
    return core::protocol_errors::UnsupportedOperation("task is already terminal");
  }
  return {};
}

core::Result<lf::a2a::v1::Task> TaskLifecycleService::TransitionTaskStatus(std::string_view task_id,
                                                                           lf::a2a::v1::TaskState next_state) const {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  const core::subscription_diagnostics::ScopedTimer timer(core::subscription_diagnostics::Phase::kTerminalStoreUpdate,
                                                          core::IsTerminalTaskState(next_state));
#endif
  if (store_ == nullptr) {
    return core::Error::Internal("TaskLifecycleService store is not configured");
  }
  auto current = store_->Get(task_id);
  if (!current.ok()) {
    return current.error();
  }
  if (core::IsTerminalTaskState(current.value().status().state())) {
    if (next_state == lf::a2a::v1::TASK_STATE_CANCELED) {
      return core::protocol_errors::TaskNotCancelable();
    }
    return core::protocol_errors::UnsupportedOperation("task is already terminal");
  }
  current.value().mutable_status()->set_state(next_state);
  const auto upsert = store_->CreateOrUpdate(current.value());
  if (!upsert.ok()) {
    return upsert.error();
  }
  return current.value();
}

core::Result<lf::a2a::v1::Task> TaskLifecycleService::AppendHistory(std::string_view task_id,
                                                                    const lf::a2a::v1::Message& message,
                                                                    TaskStore::HistoryAppendPolicy policy) const {
  if (store_ == nullptr) {
    return core::Error::Internal("TaskLifecycleService store is not configured");
  }
  return store_->AppendTaskHistory(task_id, message, policy);
}

}  // namespace a2a::server
