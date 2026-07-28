// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/list_tasks.h"

#include <google/protobuf/unknown_field_set.h>

#include <algorithm>
#include <charconv>

#include "a2a/core/error.h"

namespace a2a::server {
namespace {

bool HasStatusAfterCutoff(const lf::a2a::v1::Task& task, const google::protobuf::Timestamp& cutoff) {
  if (!task.status().has_timestamp()) {
    return false;
  }
  const auto& ts = task.status().timestamp();
  return ts.seconds() > cutoff.seconds() || (ts.seconds() == cutoff.seconds() && ts.nanos() >= cutoff.nanos());
}

void CopyUnknownFields(const lf::a2a::v1::Task& source, lf::a2a::v1::Task* destination) {
  const auto* reflection = lf::a2a::v1::Task::GetReflection();
  const auto& unknown_fields = reflection->GetUnknownFields(source);
  if (unknown_fields.empty()) {
    return;
  }

  reflection->MutableUnknownFields(destination)->MergeFrom(unknown_fields);
}

}  // namespace

bool MatchesListFilters(const lf::a2a::v1::Task& task, const ListTasksRequest& request) {
  if (!request.context_id.empty() && task.context_id() != request.context_id) {
    return false;
  }
  if (request.status_filter.has_value() && task.status().state() != *request.status_filter) {
    return false;
  }
  if (request.status_timestamp_after.has_value() && !HasStatusAfterCutoff(task, *request.status_timestamp_after)) {
    return false;
  }
  return true;
}

core::Result<std::size_t> ParseListPageToken(std::string_view page_token) {
  if (page_token.empty()) {
    return std::size_t{0};
  }
  std::size_t parsed = 0;
  const auto* begin = page_token.data();
  const auto* end = page_token.data() + page_token.size();
  const auto result = std::from_chars(begin, end, parsed);
  if (result.ec != std::errc() || result.ptr != end) {
    return core::Error::Validation("ListTasksRequest.page_token must be a non-negative integer");
  }
  return parsed;
}

core::Result<void> ValidateListPageOffset(std::size_t offset, std::size_t size) {
  if (offset > size) {
    return core::Error::Validation("ListTasksRequest.page_token exceeds available task count");
  }
  return {};
}

void ApplyArtifactProjection(lf::a2a::v1::Task* task, bool include_artifacts) {
  if (!include_artifacts) {
    task->clear_artifacts();
  }
}

lf::a2a::v1::Task ProjectTaskForList(const lf::a2a::v1::Task& task, bool include_artifacts,
                                     std::optional<std::size_t> history_length) {
  if (include_artifacts && !history_length.has_value()) {
    return task;
  }

  lf::a2a::v1::Task projected;
  CopyUnknownFields(task, &projected);
  projected.set_id(task.id());
  projected.set_context_id(task.context_id());
  if (task.has_status()) {
    *projected.mutable_status() = task.status();
  }
  if (include_artifacts) {
    *projected.mutable_artifacts() = task.artifacts();
  }

  if (!history_length.has_value()) {
    *projected.mutable_history() = task.history();
    if (task.has_metadata()) {
      *projected.mutable_metadata() = task.metadata();
    }
    return projected;
  }

  const auto history_size = static_cast<std::size_t>(task.history_size());
  const std::size_t retained_history_size = std::min(*history_length, history_size);
  projected.mutable_history()->Reserve(static_cast<int>(retained_history_size));
  const std::size_t first_history_index = history_size - retained_history_size;
  for (std::size_t index = first_history_index; index < history_size; ++index) {
    *projected.add_history() = task.history(static_cast<int>(index));
  }
  if (task.has_metadata()) {
    *projected.mutable_metadata() = task.metadata();
  }
  return projected;
}

}  // namespace a2a::server
