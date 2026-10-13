// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/task_authorization_service.h"

#include <utility>

#include "a2a/core/protocol_errors.h"

namespace a2a::server {
namespace {
constexpr std::string_view kMissingTaskStore = "authorization task store is required";
constexpr std::string_view kAuthorizationTaskMarker = "a2a.authorization";
constexpr std::string_view kInvalidAuthorizationExplanation = "authorization requires an agent status message";
constexpr std::string_view kAuthorizationNotPending = "task is not awaiting authorization";
constexpr std::string_view kAuthorizationNotGranted = "task authorization has not been granted";
}  // namespace

TaskAuthorizationService::TaskAuthorizationService(TaskStore* store, CredentialValidator validator)
    : store_(store), lifecycle_(store), validator_(std::move(validator)) {}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::RequestAuthorization(
    const lf::a2a::v1::SendMessageRequest& request, const RequestContext& context,
    const lf::a2a::v1::Message& explanation) {
  std::lock_guard lock(mutex_);
  if (store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingTaskStore));
  }
  if (explanation.role() != lf::a2a::v1::ROLE_AGENT || explanation.message_id().empty() ||
      explanation.parts().empty()) {
    return core::Error::Validation(std::string(kInvalidAuthorizationExplanation));
  }
  auto id = lifecycle_.ResolveTaskIdForSendRequest(request, context);
  if (!id.ok()) {
    return id.error();
  }
  lf::a2a::v1::Task task;
  if (!request.message().task_id().empty()) {
    auto existing = store_->Get(id.value());
    if (!existing.ok()) {
      return existing.error();
    }
    auto valid = TaskLifecycleService::ValidateTaskForSendRequest(request, existing.value());
    if (!valid.ok()) {
      return valid.error();
    }
    task = std::move(existing.value());
  } else {
    task.set_id(id.value());
    task.set_context_id(request.message().context_id().empty() ? id.value() : request.message().context_id());
  }
  (*task.mutable_metadata()->mutable_fields())[std::string(kAuthorizationTaskMarker)].set_bool_value(true);
  task.mutable_status()->set_state(lf::a2a::v1::TASK_STATE_AUTH_REQUIRED);
  *task.mutable_status()->mutable_message() = explanation;
  task.mutable_status()->mutable_message()->set_task_id(task.id());
  task.mutable_status()->mutable_message()->set_context_id(task.context_id());
  *task.add_history() = request.message();
  auto stored = lifecycle_.CreateOrUpdateTask(task);
  if (!stored.ok()) {
    return stored.error();
  }
  subscriptions_.PublishTaskUpdated(stored.value());
  return stored;
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::ReceiveMessage(
    const lf::a2a::v1::SendMessageRequest& request) {
  std::lock_guard lock(mutex_);
  if (store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingTaskStore));
  }
  auto task = store_->Get(request.message().task_id());
  if (!task.ok()) {
    return task.error();
  }
  auto valid = TaskLifecycleService::ValidateTaskForSendRequest(request, task.value());
  if (!valid.ok()) {
    return valid.error();
  }
  if (task.value().status().state() != lf::a2a::v1::TASK_STATE_AUTH_REQUIRED) {
    return core::protocol_errors::UnsupportedOperation(std::string(kAuthorizationNotPending));
  }
  auto appended =
      lifecycle_.AppendHistory(task.value().id(), request.message(), TaskStore::HistoryAppendPolicy::kDedupByMessageId);
  if (!appended.ok()) {
    return appended.error();
  }
  subscriptions_.PublishTaskUpdated(appended.value());
  return appended;
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Authorize(std::string_view task_id,
                                                                    std::string_view credential) {
  std::lock_guard lock(mutex_);
  if (store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingTaskStore));
  }
  auto task = store_->Get(task_id);
  if (!task.ok()) {
    return task.error();
  }
  if (task.value().status().state() != lf::a2a::v1::TASK_STATE_AUTH_REQUIRED) {
    return core::protocol_errors::UnsupportedOperation(std::string(kAuthorizationNotPending));
  }
  if (!validator_ || !validator_(task.value(), credential)) {
    return core::protocol_errors::PermissionDenied();
  }
  return Transition(task_id, lf::a2a::v1::TASK_STATE_WORKING);
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Transition(std::string_view task_id,
                                                                     lf::a2a::v1::TaskState state) {
  auto task = lifecycle_.TransitionTaskStatus(task_id, state);
  if (!task.ok()) {
    return task.error();
  }
  task.value().mutable_status()->clear_message();
  auto stored = lifecycle_.CreateOrUpdateTask(task.value());
  if (!stored.ok()) {
    return stored.error();
  }
  subscriptions_.PublishTaskUpdated(stored.value());
  return stored;
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Complete(std::string_view task_id) {
  std::lock_guard lock(mutex_);
  if (store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingTaskStore));
  }
  auto task = store_->Get(task_id);
  if (!task.ok()) {
    return task.error();
  }
  if (task.value().status().state() != lf::a2a::v1::TASK_STATE_WORKING) {
    return core::protocol_errors::UnsupportedOperation(std::string(kAuthorizationNotGranted));
  }
  return Transition(task_id, lf::a2a::v1::TASK_STATE_COMPLETED);
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Cancel(std::string_view task_id) {
  std::lock_guard lock(mutex_);
  return Transition(task_id, lf::a2a::v1::TASK_STATE_CANCELED);
}

core::Result<std::unique_ptr<ServerStreamSession>> TaskAuthorizationService::Subscribe(std::string_view task_id) {
  std::lock_guard lock(mutex_);
  if (store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingTaskStore));
  }
  auto task = store_->Get(task_id);
  if (!task.ok()) {
    return task.error();
  }
  return subscriptions_.Subscribe(task.value());
}
bool TaskAuthorizationService::IsAuthorizationTask(const lf::a2a::v1::Task& task) {
  const auto marker = task.metadata().fields().find(std::string(kAuthorizationTaskMarker));
  return marker != task.metadata().fields().end() && marker->second.bool_value();
}
void TaskAuthorizationService::Shutdown() { subscriptions_.Shutdown(); }

}  // namespace a2a::server
