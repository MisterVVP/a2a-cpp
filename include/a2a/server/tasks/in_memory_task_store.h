// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

struct TaskStoreStringHash final {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
    return (*this)(std::string_view(value));
  }

  [[nodiscard]] std::size_t operator()(const char* value) const noexcept { return (*this)(std::string_view(value)); }
};

struct TaskStoreStringEqual final {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
};

class InMemoryTaskStore final : public TaskStore {
 public:
  class HistoryTelemetrySink {
   public:
    virtual ~HistoryTelemetrySink() = default;
    virtual void OnDedupedHistoryMessage(const HistoryDedupeEvent& event) = 0;
  };

  InMemoryTaskStore() = default;
  explicit InMemoryTaskStore(std::shared_ptr<HistoryTelemetrySink> telemetry_sink);

  [[nodiscard]] core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override;
  [[nodiscard]] bool SupportsConditionalWrites() const noexcept override { return true; }
  [[nodiscard]] core::Result<TaskSnapshot> GetSnapshot(std::string_view id) const override;
  [[nodiscard]] core::Result<ConditionalWriteResult> CreateOrUpdateIfRevision(const lf::a2a::v1::Task& task,
                                                                              std::uint64_t expected_revision) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override;
  [[nodiscard]] core::Result<ListTasksResponse> List(const ListTasksRequest& request) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                  const lf::a2a::v1::Message& message,
                                                                  HistoryAppendPolicy policy) override;
  [[nodiscard]] HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override;

 private:
  static std::optional<std::size_t> ParsePageToken(std::string_view token);
  [[nodiscard]] core::Result<ListTasksResponse> ListUnfiltered(const ListTasksRequest& request,
                                                               std::size_t start) const;
  [[nodiscard]] core::Result<ListTasksResponse> ListFiltered(const ListTasksRequest& request, std::size_t start) const;

  mutable std::shared_mutex mutex_;
  std::shared_ptr<HistoryTelemetrySink> telemetry_sink_;
  HistoryTelemetrySnapshot telemetry_snapshot_;
  std::vector<lf::a2a::v1::Task> ordered_tasks_;
  std::vector<std::uint64_t> revisions_;
  std::unordered_map<std::string, std::size_t, TaskStoreStringHash, TaskStoreStringEqual> task_indices_;
};

}  // namespace a2a::server
