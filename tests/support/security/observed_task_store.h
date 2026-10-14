// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <functional>
#include <vector>

#include "a2a/server/tasks/in_memory_task_store.h"

namespace a2a::tests::security {

inline constexpr std::string_view kInjectedWriteFailure = "injected conditional write failure";

class ObservedTaskStore final : public server::TaskStore {
 public:
  core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override { return backing.CreateOrUpdate(task); }
  bool SupportsConditionalWrites() const noexcept override { return true; }
  core::Result<TaskSnapshot> GetSnapshot(std::string_view id) const override { return backing.GetSnapshot(id); }
  core::Result<ConditionalWriteResult> CreateOrUpdateIfRevision(const lf::a2a::v1::Task& task,
                                                                std::uint64_t revision) override {
    ++conditional_writes;
    if (fail_next_write) {
      fail_next_write = false;
      return core::Error::Internal(std::string(kInjectedWriteFailure));
    }
    if (before_write) {
      before_write();
    }
    auto result = backing.CreateOrUpdateIfRevision(task, revision);
    if (after_write) {
      after_write(task);
    }
    return result;
  }
  core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override { return backing.Get(id); }
  core::Result<server::ListTasksResponse> List(const server::ListTasksRequest& request) const override {
    list_queries.push_back(request);
    return backing.List(request);
  }
  core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override { return backing.Cancel(id); }
  core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view id, const lf::a2a::v1::Message& message,
                                                    HistoryAppendPolicy policy) override {
    return backing.AppendTaskHistory(id, message, policy);
  }
  HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override {
    return backing.GetHistoryTelemetrySnapshot();
  }

  server::InMemoryTaskStore backing;
  bool fail_next_write = false;
  std::size_t conditional_writes = 0;
  std::function<void()> before_write;
  std::function<void(const lf::a2a::v1::Task&)> after_write;
  mutable std::vector<server::ListTasksRequest> list_queries;
};
}  // namespace a2a::tests::security
