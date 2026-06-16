// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/in_memory_task_store.h"

#include <charconv>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/task_states.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/server/tasks/task_history.h"

namespace a2a::server {

InMemoryTaskStore::InMemoryTaskStore(std::shared_ptr<HistoryTelemetrySink> telemetry_sink)
    : telemetry_sink_(std::move(telemetry_sink)) {}

core::Result<void> InMemoryTaskStore::CreateOrUpdate(const lf::a2a::v1::Task& task) {
  if (task.id().empty()) {
    return core::Error::Validation("Task.id is required");
  }

  std::unique_lock<std::shared_mutex> lock(mutex_);
  auto [it, inserted] = tasks_.try_emplace(task.id(), task);
  if (inserted) {
    ordered_ids_.push_back(it->first);
  } else {
    it->second = task;
  }
  return {};
}

core::Result<lf::a2a::v1::Task> InMemoryTaskStore::Get(std::string_view id) const {
  if (id.empty()) {
    return core::Error::Validation("Task id is required");
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  const auto it = tasks_.find(id);
  if (it == tasks_.end()) {
    return core::protocol_errors::TaskNotFound("Task not found");
  }
  return it->second;
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
  const std::size_t effective_page_size = request.page_size;
  ListTasksResponse response;
  if (effective_page_size == 0) {
    response.tasks.reserve(start < ordered_ids_.size() ? ordered_ids_.size() - start : 0);
  } else {
    response.tasks.reserve(effective_page_size);
  }

  std::size_t matched_count = 0;
  for (const auto& id : ordered_ids_) {
    const auto it = tasks_.find(id);
    if (it != tasks_.end() && MatchesListFilters(it->second, request)) {
      if (matched_count >= start && (effective_page_size == 0 || response.tasks.size() < effective_page_size)) {
        lf::a2a::v1::Task projected = it->second;
        ApplyArtifactProjection(&projected, request.include_artifacts);
        ApplyHistoryRetention(&projected, request.history_length);
        response.tasks.push_back(std::move(projected));
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
  const auto it = tasks_.find(id);
  if (it == tasks_.end()) {
    return core::protocol_errors::TaskNotFound("Task not found");
  }
  if (core::IsTerminalTaskState(it->second.status().state())) {
    return core::protocol_errors::TaskNotCancelable();
  }

  auto* mutable_status = it->second.mutable_status();
  mutable_status->set_state(lf::a2a::v1::TASK_STATE_CANCELED);
  return it->second;
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
    const auto it = tasks_.find(task_id);
    if (it == tasks_.end()) {
      return core::protocol_errors::TaskNotFound("Task not found");
    }

    const auto dedupe_reason = FindHistoryDedupeReason(it->second.history(), message, policy);
    if (dedupe_reason.has_value()) {
      UpdateDedupeSnapshot(&telemetry_snapshot_, *dedupe_reason);
      telemetry_sink = telemetry_sink_;
      dedupe_event = TaskStore::HistoryDedupeEvent{
          .task_id = std::string(task_id),
          .message_id = message.message_id(),
          .policy = policy,
          .reason = *dedupe_reason,
      };
      result = it->second;
    } else {
      *it->second.add_history() = message;
      result = it->second;
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
