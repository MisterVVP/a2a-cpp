// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/list_tasks.h"

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

}  // namespace a2a::server
