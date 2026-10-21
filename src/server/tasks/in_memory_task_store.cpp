// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/in_memory_task_store.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <utility>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/non_copyable.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_history.h"

namespace a2a::server {
namespace {

constexpr std::uint64_t kInitialTaskRevision = 1;

class TaskAppendRollback final : private core::NonCopyableOrMovable {
 public:
  TaskAppendRollback(std::vector<lf::a2a::v1::Task>* tasks, std::vector<std::uint64_t>* revisions)
      : tasks_(tasks), revisions_(revisions) {}
  ~TaskAppendRollback() {
    if (tasks_ != nullptr) {
      tasks_->pop_back();
      revisions_->pop_back();
    }
  }

  void Commit() noexcept { tasks_ = nullptr; }

 private:
  std::vector<lf::a2a::v1::Task>* tasks_;
  std::vector<std::uint64_t>* revisions_;
};

}  // namespace

InMemoryTaskStore::InMemoryTaskStore()
    : InMemoryTaskStore(nullptr, [] {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
      }) {}

InMemoryTaskStore::InMemoryTaskStore(std::shared_ptr<HistoryTelemetrySink> telemetry_sink, UpdateClock update_clock)
    : update_clock_(std::move(update_clock)), telemetry_sink_(std::move(telemetry_sink)) {
  assert(update_clock_);
}

InMemoryTaskStore::InMemoryTaskStore(std::shared_ptr<HistoryTelemetrySink> telemetry_sink) : InMemoryTaskStore() {
  telemetry_sink_ = std::move(telemetry_sink);
}

void InMemoryTaskStore::TouchTask(std::size_t index) {
  const auto& id = ordered_tasks_[index].id();
  if (index == updated_at_.size()) {
    const auto now = std::max(std::int64_t{0}, update_clock_());
    update_order_.insert({.updated_at = now, .task_id = id});
    updated_at_.push_back(now);
    return;
  }
  const auto next_time = std::max(updated_at_[index] + 1, update_clock_());
  auto node = update_order_.extract({.updated_at = updated_at_[index], .task_id = id});
  node.value().updated_at = next_time;
  update_order_.insert(std::move(node));
  updated_at_[index] = next_time;
}

core::Result<void> InMemoryTaskStore::CreateOrUpdate(const lf::a2a::v1::Task& task) {
  if (task.id().empty()) {
    return core::Error::Validation("Task.id is required");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto existing = task_indices_.find(task.id());
  if (existing != task_indices_.end()) {
    ordered_tasks_[existing->second] = task;
    ++revisions_[existing->second];
    TouchTask(existing->second);
    return {};
  }

  ordered_tasks_.push_back(task);
  revisions_.push_back(kInitialTaskRevision);
  TaskAppendRollback rollback(&ordered_tasks_, &revisions_);
  const bool inserted = task_indices_.try_emplace(task.id(), ordered_tasks_.size() - 1).second;
  if (!inserted) {
    return core::Error::Internal("Task index insertion failed");
  }

  TouchTask(ordered_tasks_.size() - 1);
  rollback.Commit();
  return {};
}

core::Result<TaskStore::TaskSnapshot> InMemoryTaskStore::GetSnapshot(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation("Task id is required");
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = task_indices_.find(id);
  if (it == task_indices_.end()) {
    return core::protocol_errors::TaskNotFound("Task not found");
  }
  return TaskSnapshot{.task = ordered_tasks_[it->second], .revision = revisions_[it->second]};
}

core::Result<TaskStore::ConditionalWriteResult> InMemoryTaskStore::CreateOrUpdateIfRevision(
    const lf::a2a::v1::Task& task, std::uint64_t expected_revision) {
  if (task.id().empty()) {
    return core::Error::Validation("Task.id is required");
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto existing = task_indices_.find(task.id());
  if (existing == task_indices_.end()) {
    if (expected_revision != 0) {
      return ConditionalWriteResult::kConflict;
    }
    ordered_tasks_.push_back(task);
    revisions_.push_back(kInitialTaskRevision);
    TaskAppendRollback rollback(&ordered_tasks_, &revisions_);
    if (!task_indices_.try_emplace(task.id(), ordered_tasks_.size() - 1).second) {
      return core::Error::Internal("Task index insertion failed");
    }
    TouchTask(ordered_tasks_.size() - 1);
    rollback.Commit();
    return ConditionalWriteResult::kUpdated;
  }
  if (revisions_[existing->second] != expected_revision) {
    return ConditionalWriteResult::kConflict;
  }
  ordered_tasks_[existing->second] = task;
  ++revisions_[existing->second];
  TouchTask(existing->second);
  return ConditionalWriteResult::kUpdated;
}

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::Get(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = task_indices_.find(id);
  if (it == task_indices_.end()) {
    return core::protocol_errors::TaskNotFound("Task not found");
  }
  return ordered_tasks_[it->second];
}

core::Result<ListTasksResponse> InMemoryTaskStore::List(const ListTasksRequest& request) const {
  const auto cursor = ParseListPageToken(request.page_token);
  if (!cursor.ok()) {
    return cursor.error();
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  ListTasksResponse response;
  const auto capacity =
      request.page_size == 0 ? ordered_tasks_.size() : std::min(request.page_size, ordered_tasks_.size());
  response.tasks.reserve(capacity);
  response.task_cursors.reserve(capacity);
  const bool filtered =
      !request.context_id.empty() || request.status_filter.has_value() || request.status_timestamp_after.has_value();
  if (filtered && request.include_total_size) {
    for (const auto& task : ordered_tasks_) {
      response.total_size += MatchesListFilters(task, request) ? 1U : 0U;
    }
  } else if (request.include_total_size) {
    response.total_size = ordered_tasks_.size();
  }
  const auto& boundary = cursor.value();
  auto it = update_order_.begin();
  if (boundary.has_value()) {
    it = update_order_.upper_bound(*boundary);
  }
  for (; it != update_order_.end(); ++it) {
    const auto& task = ordered_tasks_[task_indices_.at(it->task_id)];
    if (!MatchesListFilters(task, request)) {
      continue;
    }
    if (request.page_size != 0 && response.tasks.size() == request.page_size) {
      response.next_page_token = EncodeListPageToken(response.task_cursors.back());
      break;
    }
    response.tasks.push_back(ProjectTaskForList(task, request.include_artifacts, request.history_length));
    response.task_cursors.push_back(*it);
  }
  response.page_size = response.tasks.size();
  return response;
}

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::Cancel(std::string_view id) {
  if (id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto it = task_indices_.find(id);
  if (it == task_indices_.end()) {
    return core::protocol_errors::TaskNotFound("Task not found");
  }
  auto& task = ordered_tasks_[it->second];
  if (core::IsTerminalTaskState(task.status().state())) {
    return core::protocol_errors::TaskNotCancelable();
  }

  auto* mutable_status = task.mutable_status();
  mutable_status->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
  ++revisions_[it->second];
  TouchTask(it->second);
  return task;
}

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::AppendTaskHistory(std::string_view task_id,
                                                                     const lf::a2a::v1::Message& message,
                                                                     HistoryAppendPolicy policy) {
  if (task_id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::shared_ptr<HistoryTelemetrySink> telemetry_sink;
  std::optional<HistoryDedupeEvent> dedupe_event;
  lf::a2a::v1::Task result;
  {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = task_indices_.find(task_id);
    if (it == task_indices_.end()) {
      return core::protocol_errors::TaskNotFound("Task not found");
    }

    auto& task = ordered_tasks_[it->second];
    const auto dedupe_reason = FindHistoryDedupeReason(task.history(), message, policy);
    if (dedupe_reason.has_value()) {
      UpdateDedupeSnapshot(&telemetry_snapshot_, *dedupe_reason);
      telemetry_sink = telemetry_sink_;
      dedupe_event = TaskStore::HistoryDedupeEvent{
          .task_id = std::string(task_id),
          .message_id = message.message_id(),
          .policy = policy,
          .reason = *dedupe_reason,
      };
      result = task;
    } else {
      *task.add_history() = message;
      ++revisions_[it->second];
      TouchTask(it->second);
      result = task;
    }
  }

  if (telemetry_sink != nullptr && dedupe_event.has_value()) {
    telemetry_sink->OnDedupedHistoryMessage(*dedupe_event);
  }
  return result;
}

TaskStore::HistoryTelemetrySnapshot InMemoryTaskStore::GetHistoryTelemetrySnapshot() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return telemetry_snapshot_;
}

}  // namespace a2a::server
