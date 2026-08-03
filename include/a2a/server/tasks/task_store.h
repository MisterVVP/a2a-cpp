// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "a2a/core/error.h"
#include "a2a/core/result.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

inline constexpr std::string_view kConditionalTaskPersistenceUnsupportedMessage =
    "conditional task persistence is not supported";

class TaskStore {
 public:
  struct TaskSnapshot final {
    lf::a2a::v1::Task task;
    std::uint64_t revision = 0;
  };

  enum class ConditionalWriteResult : std::uint8_t {
    kUpdated,
    kConflict,
  };

  enum class HistoryAppendPolicy {
    // Appends every request in arrival order (no dedupe).
    kNoDedup,
    // Drops a request only when message_id is present and both message_id + message fingerprint match.
    kDedupByMessageId,
    // Drops by message_id+fingerprint when message_id is present; otherwise drops by fingerprint alone.
    kDedupByIdOrFingerprint,
  };

  struct HistoryDedupeEvent final {
    enum class Reason : std::uint8_t {
      kDuplicateMessageIdAndFingerprint,
      kDuplicateFingerprintWithoutMessageId,
    };

    std::string task_id;
    std::string message_id;
    HistoryAppendPolicy policy = HistoryAppendPolicy::kNoDedup;
    Reason reason = Reason::kDuplicateMessageIdAndFingerprint;
  };

  struct HistoryTelemetrySnapshot final {
    std::size_t dedupe_dropped_total = 0;
    std::size_t dedupe_dropped_by_message_id_and_fingerprint = 0;
    std::size_t dedupe_dropped_by_fingerprint_without_message_id = 0;
  };

  virtual ~TaskStore() = default;

  [[nodiscard]] virtual core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) = 0;
  [[nodiscard]] virtual bool SupportsConditionalWrites() const noexcept { return false; }
  [[nodiscard]] virtual core::Result<TaskSnapshot> GetSnapshot(std::string_view id) const {
    (void)id;
    return core::Error::Internal(std::string(kConditionalTaskPersistenceUnsupportedMessage));
  }
  [[nodiscard]] virtual core::Result<ConditionalWriteResult> CreateOrUpdateIfRevision(const lf::a2a::v1::Task& task,
                                                                                      std::uint64_t expected_revision) {
    (void)task;
    (void)expected_revision;
    return core::Error::Internal(std::string(kConditionalTaskPersistenceUnsupportedMessage));
  }
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> Get(std::string_view id) const = 0;
  [[nodiscard]] virtual core::Result<ListTasksResponse> List(const ListTasksRequest& request) const = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                          const lf::a2a::v1::Message& message,
                                                                          HistoryAppendPolicy policy) = 0;
  [[nodiscard]] virtual HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const = 0;
};

}  // namespace a2a::server
