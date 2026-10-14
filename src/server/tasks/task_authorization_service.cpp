// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/task_authorization_service.h"

#include <utility>

#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"
#include "a2a/server/tasks/task_history.h"

namespace a2a::server {
namespace {
constexpr std::string_view kMissingTaskStore = "authorization task store is required";
constexpr std::string_view kAuthorizationTaskMarker = "a2a.authorization";
constexpr std::string_view kInvalidAuthorizationExplanation = "authorization requires an agent status message";
constexpr std::string_view kAuthorizationNotPending = "task is not awaiting authorization";
constexpr std::string_view kTaskChanged = "task changed while updating authorization; retry the operation";
constexpr std::string_view kAuthorizationNotGranted = "task authorization has not been granted";
}  // namespace

TaskAuthorizationService::TaskAuthorizationService(TaskStore* store, CredentialValidator validator)
    : store_(store), lifecycle_(store), validator_(std::move(validator)) {}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::RequestAuthorization(
    const lf::a2a::v1::SendMessageRequest& request, const RequestContext& context,
    const lf::a2a::v1::Message& explanation) {
  std::lock_guard lock(mutex_);
  return RequestAuthorizationLocked(request, context, explanation);
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::RequestAuthorizationLocked(
    const lf::a2a::v1::SendMessageRequest& request, const RequestContext& context,
    const lf::a2a::v1::Message& explanation) {
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
  TaskStore::TaskSnapshot snapshot;
  auto& task = snapshot.task;
  if (!request.message().task_id().empty()) {
    auto existing = Snapshot(id.value());
    if (!existing.ok()) {
      return existing.error();
    }
    auto valid = TaskLifecycleService::ValidateTaskForSendRequest(request, existing.value().task);
    if (!valid.ok()) {
      return valid.error();
    }
    snapshot = std::move(existing.value());
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
  return Persist(std::move(snapshot));
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::ReceiveMessage(
    const lf::a2a::v1::SendMessageRequest& request) {
  std::lock_guard lock(mutex_);
  return ReceiveMessageLocked(request);
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::ReceiveMessageLocked(
    const lf::a2a::v1::SendMessageRequest& request) {
  auto snapshot = Snapshot(request.message().task_id());
  if (!snapshot.ok()) {
    return snapshot.error();
  }
  auto& task = snapshot.value().task;
  auto valid = TaskLifecycleService::ValidateTaskForSendRequest(request, task);
  if (!valid.ok()) {
    return valid.error();
  }
  if (!IsAuthorizationTask(task) || task.status().state() != lf::a2a::v1::TASK_STATE_AUTH_REQUIRED) {
    return core::protocol_errors::UnsupportedOperation(std::string(kAuthorizationNotPending));
  }
  if (FindHistoryDedupeReason(task.history(), request.message(), TaskStore::HistoryAppendPolicy::kDedupByMessageId)) {
    return task;
  }
  *task.add_history() = request.message();
  return Persist(std::move(snapshot.value()));
}

core::Result<TaskStore::TaskSnapshot> TaskAuthorizationService::Snapshot(std::string_view task_id) const {
  if (store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingTaskStore));
  }
  return store_->GetSnapshot(task_id);
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Persist(TaskStore::TaskSnapshot snapshot) {
  auto stored = store_->CreateOrUpdateIfRevision(snapshot.task, snapshot.revision);
  if (!stored.ok()) {
    return stored.error();
  }
  if (stored.value() == TaskStore::ConditionalWriteResult::kConflict) {
    return core::protocol_errors::UnsupportedOperation(std::string(kTaskChanged));
  }
  subscriptions_.PublishTaskUpdated(snapshot.task);
  return std::move(snapshot.task);
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Authorize(std::string_view task_id,
                                                                    std::string_view credential) {
  std::lock_guard lock(mutex_);
  if (store_ == nullptr) {
    return core::Error::Internal(std::string(kMissingTaskStore));
  }
  auto snapshot = Snapshot(task_id);
  if (!snapshot.ok()) {
    return snapshot.error();
  }
  const auto& task = snapshot.value().task;
  if (!IsAuthorizationTask(task) || task.status().state() != lf::a2a::v1::TASK_STATE_AUTH_REQUIRED) {
    return core::protocol_errors::UnsupportedOperation(std::string(kAuthorizationNotPending));
  }
  if (!validator_ || !validator_(task, credential)) {
    return core::protocol_errors::PermissionDenied();
  }
  return Transition(std::move(snapshot.value()), lf::a2a::v1::TASK_STATE_WORKING);
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Transition(TaskStore::TaskSnapshot snapshot,
                                                                     lf::a2a::v1::TaskState state) {
  if (core::IsTerminalTaskState(snapshot.task.status().state())) {
    return state == lf::a2a::v1::TASK_STATE_CANCELED ? core::protocol_errors::TaskNotCancelable()
                                                     : core::protocol_errors::UnsupportedOperation();
  }
  snapshot.task.mutable_status()->set_state(state);
  snapshot.task.mutable_status()->clear_message();
  return Persist(std::move(snapshot));
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Complete(std::string_view task_id) {
  std::lock_guard lock(mutex_);
  auto snapshot = Snapshot(task_id);
  if (!snapshot.ok()) {
    return snapshot.error();
  }
  if (!IsAuthorizationTask(snapshot.value().task) ||
      snapshot.value().task.status().state() != lf::a2a::v1::TASK_STATE_WORKING) {
    return core::protocol_errors::UnsupportedOperation(std::string(kAuthorizationNotGranted));
  }
  return Transition(std::move(snapshot.value()), lf::a2a::v1::TASK_STATE_COMPLETED);
}

core::Result<lf::a2a::v1::Task> TaskAuthorizationService::Cancel(std::string_view task_id) {
  std::lock_guard lock(mutex_);
  auto snapshot = Snapshot(task_id);
  if (!snapshot.ok()) {
    return snapshot.error();
  }
  return Transition(std::move(snapshot.value()), lf::a2a::v1::TASK_STATE_CANCELED);
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
core::Result<std::unique_ptr<ServerStreamSession>> TaskAuthorizationService::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, const RequestContext& context,
    const lf::a2a::v1::Message& explanation) {
  std::lock_guard lock(mutex_);
  auto task = request.message().task_id().empty() ? RequestAuthorizationLocked(request, context, explanation)
                                                  : ReceiveMessageLocked(request);
  if (!task.ok()) {
    return task.error();
  }
  // Register before Authorize/Complete can acquire this service's mutex.
  return subscriptions_.Subscribe(task.value());
}

bool TaskAuthorizationService::IsAuthorizationTask(const lf::a2a::v1::Task& task) {
  const auto marker = task.metadata().fields().find(std::string(kAuthorizationTaskMarker));
  return marker != task.metadata().fields().end() && marker->second.bool_value();
}
void TaskAuthorizationService::Shutdown() { subscriptions_.Shutdown(); }

}  // namespace a2a::server
