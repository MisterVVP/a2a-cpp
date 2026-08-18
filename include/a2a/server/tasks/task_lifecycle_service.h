// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "a2a/core/result.h"
#include "a2a/server/request_context.h"
#include "a2a/server/task_id_generator.h"
#include "a2a/server/tasks/task_store.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

class TaskLifecycleService final {
 public:
  explicit TaskLifecycleService(TaskStore* store, std::shared_ptr<TaskIdGenerator> task_id_generator = nullptr);

  [[nodiscard]] core::Result<lf::a2a::v1::Task> CreateOrUpdateTask(const lf::a2a::v1::Task& task) const;
  // Resolves only the identifier. Callers must validate an authoritative task
  // snapshot after taking their task-level synchronization.
  [[nodiscard]] core::Result<std::string> ResolveTaskIdForSendRequest(const lf::a2a::v1::SendMessageRequest& request,
                                                                      const RequestContext& context) const;
  [[nodiscard]] static core::Result<void> ValidateTaskForSendRequest(const lf::a2a::v1::SendMessageRequest& request,
                                                                     const lf::a2a::v1::Task& task);
  [[nodiscard]] core::Result<lf::a2a::v1::Task> TransitionTaskStatus(std::string_view task_id,
                                                                     lf::a2a::v1::TaskState next_state) const;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> AppendHistory(std::string_view task_id,
                                                              const lf::a2a::v1::Message& message,
                                                              TaskStore::HistoryAppendPolicy policy) const;

 private:
  TaskStore* store_ = nullptr;
  std::shared_ptr<TaskIdGenerator> task_id_generator_;
};

}  // namespace a2a::server
