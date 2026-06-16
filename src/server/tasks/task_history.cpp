// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/tasks/task_history.h"

#include <compare>

namespace a2a::server {
namespace {

void ApplyHistoryLimit(lf::a2a::v1::Task* task, std::size_t keep) {
  if (keep == 0) {
    task->clear_history();
    return;
  }
  if (std::cmp_less_equal(task->history_size(), keep)) {
    return;
  }
  const auto history_size = static_cast<std::size_t>(task->history_size());
  const int remove_count = static_cast<int>(history_size - keep);
  task->mutable_history()->DeleteSubrange(0, remove_count);
}

bool HasSameMessageFingerprint(const lf::a2a::v1::Message& existing, const lf::a2a::v1::Message& message) {
  if (existing.task_id() != message.task_id() || existing.context_id() != message.context_id() ||
      existing.role() != message.role() || existing.parts_size() != message.parts_size()) {
    return false;
  }

  for (int index = 0; index < existing.parts_size(); ++index) {
    if (existing.parts(index).SerializeAsString() != message.parts(index).SerializeAsString()) {
      return false;
    }
  }
  return true;
}

bool HasSameMessageIdAndFingerprint(const lf::a2a::v1::Message& existing, const lf::a2a::v1::Message& message) {
  return !existing.message_id().empty() && existing.message_id() == message.message_id() &&
         HasSameMessageFingerprint(existing, message);
}

std::optional<TaskStore::HistoryDedupeEvent::Reason> FindMessageIdDedupeReason(
    const google::protobuf::RepeatedPtrField<lf::a2a::v1::Message>& history, const lf::a2a::v1::Message& message) {
  for (const auto& existing : history) {
    if (HasSameMessageIdAndFingerprint(existing, message)) {
      return TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint;
    }
  }
  return std::nullopt;
}

std::optional<TaskStore::HistoryDedupeEvent::Reason> FindIdOrFingerprintDedupeReason(
    const google::protobuf::RepeatedPtrField<lf::a2a::v1::Message>& history, const lf::a2a::v1::Message& message) {
  const bool has_message_id = !message.message_id().empty();
  for (const auto& existing : history) {
    if (has_message_id && HasSameMessageIdAndFingerprint(existing, message)) {
      return TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint;
    }
    if (!has_message_id && HasSameMessageFingerprint(existing, message)) {
      return TaskStore::HistoryDedupeEvent::Reason::kDuplicateFingerprintWithoutMessageId;
    }
  }
  return std::nullopt;
}

}  // namespace

void ApplyHistoryRetention(lf::a2a::v1::Task* task, std::optional<std::size_t> history_length) {
  if (!history_length.has_value()) {
    return;
  }
  ApplyHistoryLimit(task, *history_length);
}

std::optional<TaskStore::HistoryDedupeEvent::Reason> FindHistoryDedupeReason(
    const google::protobuf::RepeatedPtrField<lf::a2a::v1::Message>& history, const lf::a2a::v1::Message& message,
    TaskStore::HistoryAppendPolicy policy) {
  if (policy == TaskStore::HistoryAppendPolicy::kDedupByMessageId && !message.message_id().empty()) {
    return FindMessageIdDedupeReason(history, message);
  }
  if (policy == TaskStore::HistoryAppendPolicy::kDedupByIdOrFingerprint) {
    return FindIdOrFingerprintDedupeReason(history, message);
  }
  return std::nullopt;
}

void UpdateDedupeSnapshot(TaskStore::HistoryTelemetrySnapshot* snapshot, TaskStore::HistoryDedupeEvent::Reason reason) {
  snapshot->dedupe_dropped_total += 1;
  if (reason == TaskStore::HistoryDedupeEvent::Reason::kDuplicateMessageIdAndFingerprint) {
    snapshot->dedupe_dropped_by_message_id_and_fingerprint += 1;
    return;
  }
  snapshot->dedupe_dropped_by_fingerprint_without_message_id += 1;
}

}  // namespace a2a::server
