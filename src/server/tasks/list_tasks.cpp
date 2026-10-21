// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/list_tasks.h"

#include <google/protobuf/unknown_field_set.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>

#include "a2a/core/error.h"
#include "a2a/core/protojson.h"

namespace a2a::server {
namespace {

constexpr std::string_view kCursorPrefix = "a2a1_";
constexpr std::string_view kHexDigits = "0123456789abcdef";
constexpr std::size_t kHexByteWidth = 2;
constexpr unsigned int kHexShift = 4;
constexpr unsigned int kHexMask = 15;
constexpr char kCursorSeparator = ':';
constexpr std::string_view kInvalidCursor = "ListTasksRequest.page_token is not a valid v1 cursor; restart pagination";

constexpr std::string_view kEmptyTasksJsonMember = R"("tasks":[])";
constexpr std::string_view kZeroPageSizeJsonMember = R"("pageSize":0)";
constexpr std::string_view kZeroTotalSizeJsonMember = R"("totalSize":0)";
constexpr std::string_view kEmptyNextPageTokenJsonMember = R"("nextPageToken":"")";

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

core::Result<std::size_t> NormalizeListTasksPageSize(std::optional<std::int64_t> page_size) {
  if (!page_size.has_value()) {
    return kDefaultListTasksPageSize;
  }
  if (*page_size < 1 || std::cmp_greater(*page_size, kMaxListTasksPageSize)) {
    return core::Error::Validation("ListTasksRequest.page_size must be between 1 and 100");
  }
  return static_cast<std::size_t>(*page_size);
}

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

bool ListCursorPrecedes(const ListTaskCursor& lhs, const ListTaskCursor& rhs) {
  return lhs.updated_at != rhs.updated_at ? lhs.updated_at > rhs.updated_at : lhs.task_id > rhs.task_id;
}

std::string EncodeListPageToken(const ListTaskCursor& cursor) {
  std::string payload = std::to_string(cursor.updated_at);
  payload.push_back(kCursorSeparator);
  payload.append(cursor.task_id);
  std::string token(kCursorPrefix);
  token.reserve(token.size() + (payload.size() * kHexByteWidth));
  for (const char character : payload) {
    const auto byte = static_cast<unsigned char>(character);
    token.push_back(kHexDigits[byte >> kHexShift]);
    token.push_back(kHexDigits[byte & kHexMask]);
  }
  return token;
}

core::Result<std::optional<ListTaskCursor>> ParseListPageToken(std::string_view page_token) {
  if (page_token.empty()) {
    return std::optional<ListTaskCursor>{};
  }
  if (!page_token.starts_with(kCursorPrefix)) {
    return core::Error::Validation(std::string(kInvalidCursor));
  }
  page_token.remove_prefix(kCursorPrefix.size());
  if (page_token.size() % kHexByteWidth != 0) {
    return core::Error::Validation(std::string(kInvalidCursor));
  }
  std::string payload;
  payload.reserve(page_token.size() / kHexByteWidth);
  for (std::size_t index = 0; index < page_token.size(); index += kHexByteWidth) {
    const auto high = kHexDigits.find(page_token[index]);
    const auto low = kHexDigits.find(page_token[index + 1]);
    if (high == std::string_view::npos || low == std::string_view::npos) {
      return core::Error::Validation(std::string(kInvalidCursor));
    }
    payload.push_back(static_cast<char>((high << kHexShift) | low));
  }
  const auto separator = payload.find(kCursorSeparator);
  if (separator == std::string::npos || separator + 1 == payload.size()) {
    return core::Error::Validation(std::string(kInvalidCursor));
  }
  ListTaskCursor cursor;
  const auto parsed = std::from_chars(payload.data(), payload.data() + separator, cursor.updated_at);
  if (parsed.ec != std::errc{} || parsed.ptr != payload.data() + separator || cursor.updated_at < 0) {
    return core::Error::Validation(std::string(kInvalidCursor));
  }
  cursor.task_id = payload.substr(separator + 1);
  // Canonical encoding rejects aliases, NULs and malformed numeric components.
  if (cursor.task_id.find('\0') != std::string::npos ||
      EncodeListPageToken(cursor) != std::string(kCursorPrefix).append(page_token)) {
    return core::Error::Validation(std::string(kInvalidCursor));
  }
  return std::optional<ListTaskCursor>{std::move(cursor)};
}

core::Result<std::string> SerializeListTasksResponse(const ListTasksResponse& response) {
  constexpr std::size_t kClosingBraceSize = 1U;
  constexpr auto kMaximumProtoSize = static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());
  if (response.tasks.size() > kMaximumProtoSize || response.page_size > kMaximumProtoSize ||
      response.total_size > kMaximumProtoSize) {
    return core::Error::Serialization("ListTasksResponse value exceeds the protobuf int32 range");
  }

  lf::a2a::v1::ListTasksResponse payload;
  payload.mutable_tasks()->Reserve(static_cast<int>(response.tasks.size()));
  for (const auto& task : response.tasks) {
    *payload.add_tasks() = task;
  }
  payload.set_page_size(static_cast<std::int32_t>(response.page_size));
  payload.set_total_size(static_cast<std::int32_t>(response.total_size));
  payload.set_next_page_token(response.next_page_token);
  auto json = core::MessageToJson(payload);
  if (!json.ok()) {
    return json.error();
  }

  // Only zero-valued fields are absent. Insert those top-level members rather
  // than enabling protobuf's recursive default-field printing.
  std::string missing_fields;
  const auto append_field = [&missing_fields](std::string_view field) {
    if (!missing_fields.empty()) {
      missing_fields.push_back(',');
    }
    missing_fields.append(field);
  };
  if (response.tasks.empty()) {
    append_field(kEmptyTasksJsonMember);
  }
  if (response.page_size == 0U) {
    append_field(kZeroPageSizeJsonMember);
  }
  if (response.total_size == 0U) {
    append_field(kZeroTotalSizeJsonMember);
  }
  if (response.next_page_token.empty()) {
    append_field(kEmptyNextPageTokenJsonMember);
  }
  if (missing_fields.empty()) {
    return json;
  }
  if (json.value().size() > kClosingBraceSize + 1U) {
    missing_fields.insert(missing_fields.begin(), ',');
  }
  json.value().insert(json.value().size() - kClosingBraceSize, missing_fields);
  return json;
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
