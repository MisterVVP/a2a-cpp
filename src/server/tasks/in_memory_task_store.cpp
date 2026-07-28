// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/in_memory_task_store.h"

#include <algorithm>
#include <charconv>
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

class TaskAppendRollback final : private core::NonCopyableOrMovable {
 public:
  explicit TaskAppendRollback(std::vector<lf::a2a::v1::Task>* tasks) : tasks_(tasks) {}
  ~TaskAppendRollback() {
    if (tasks_ != nullptr) {
      tasks_->pop_back();
    }
  }

  void Commit() noexcept { tasks_ = nullptr; }

 private:
  std::vector<lf::a2a::v1::Task>* tasks_;
};

}  // namespace

InMemoryTaskStore::InMemoryTaskStore(std::shared_ptr<HistoryTelemetrySink> telemetry_sink)
    : telemetry_sink_(std::move(telemetry_sink)) {}

core::Result<void> InMemoryTaskStore::CreateOrUpdate(const lf::a2a::v1::Task& task) {
  if (task.id().empty()) {
    return core::Error::Validation("Task.id is required");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  const auto existing = task_indices_.find(task.id());
  if (existing != task_indices_.end()) {
    ordered_tasks_[existing->second] = task;
    return {};
  }

  ordered_tasks_.push_back(task);
  TaskAppendRollback rollback(&ordered_tasks_);
  const bool inserted = task_indices_.try_emplace(task.id(), ordered_tasks_.size() - 1).second;
  if (!inserted) {
    return core::Error::Internal("Task index insertion failed");
  }

  rollback.Commit();
  return {};
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

std::optional<std::size_t> InMemoryTaskStore::ParsePageToken(std::string_view token) {
  if (token.empty()) {
    return std::size_t{0};
  }

  std::size_t parsed = 0;
  const auto* begin = token.data();
  const auto* end = token.data() + token.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return std::nullopt;
  }
  return parsed;
}

core::Result<ListTasksResponse> InMemoryTaskStore::List(const ListTasksRequest& request) const {
  const auto offset = ParseListPageToken(request.page_token);
  if (!offset.ok()) {
    return offset.error();
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  const std::size_t start = offset.value();
  const bool has_filters =
      !request.context_id.empty() || request.status_filter.has_value() || request.status_timestamp_after.has_value();
  return has_filters ? ListFiltered(request, start) : ListUnfiltered(request, start);
}

core::Result<ListTasksResponse> InMemoryTaskStore::ListUnfiltered(const ListTasksRequest& request,
                                                                  std::size_t start) const {
  const auto valid_offset = ValidateListPageOffset(start, ordered_tasks_.size());
  if (!valid_offset.ok()) {
    return valid_offset.error();
  }

  const std::size_t available = ordered_tasks_.size() - start;
  const std::size_t result_size = request.page_size == 0 ? available : std::min(request.page_size, available);
  ListTasksResponse response;
  response.tasks.reserve(result_size);
  const auto end = ordered_tasks_.begin() + static_cast<std::ptrdiff_t>(start + result_size);
  for (auto it = ordered_tasks_.begin() + static_cast<std::ptrdiff_t>(start); it != end; ++it) {
    response.tasks.push_back(ProjectTaskForList(*it, request.include_artifacts, request.history_length));
  }
  response.page_size = result_size;
  response.total_size = ordered_tasks_.size();
  if (start + result_size < ordered_tasks_.size()) {
    response.next_page_token = std::to_string(start + result_size);
  }
  return response;
}

core::Result<ListTasksResponse> InMemoryTaskStore::ListFiltered(const ListTasksRequest& request,
                                                                std::size_t start) const {
  const std::size_t effective_page_size = request.page_size;
  ListTasksResponse response;

  if (effective_page_size == 0) {
    response.tasks.reserve(start < ordered_tasks_.size() ? ordered_tasks_.size() - start : 0);
  } else {
    response.tasks.reserve(effective_page_size);
  }

  std::size_t matched_count = 0;
  for (const auto& task : ordered_tasks_) {
    if (MatchesListFilters(task, request)) {
      if (matched_count >= start && (effective_page_size == 0 || response.tasks.size() < effective_page_size)) {
        response.tasks.push_back(ProjectTaskForList(task, request.include_artifacts, request.history_length));
      }
      ++matched_count;
    }
  }

  const auto valid_offset = ValidateListPageOffset(start, matched_count);
  if (!valid_offset.ok()) {
    return valid_offset.error();
  }

  response.page_size = response.tasks.size();
  response.total_size = matched_count;
  if (effective_page_size != 0 && response.tasks.size() == effective_page_size) {
    const std::size_t next_offset = start + response.tasks.size();
    if (next_offset < matched_count) {
      response.next_page_token = std::to_string(next_offset);
    }
  }

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
