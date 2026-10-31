// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "a2a/core/result.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/request_context.h"
#include "a2a/server/tasks/task_store.h"

namespace a2a::server {

class SecurityPolicy {
 public:
  virtual ~SecurityPolicy() = default;
  [[nodiscard]] virtual core::Result<std::string> Authenticate(const RequestContext& context) const = 0;
  [[nodiscard]] virtual bool AuthorizeOperation(const DispatchRequest& request,
                                                const RequestContext& context) const = 0;
  [[nodiscard]] virtual bool CanAccessTask(const lf::a2a::v1::Task& task, const RequestContext& context) const = 0;
  // May narrow store filters and projections when those fields suffice for
  // CanAccessTask. nullopt means the requested scope contains no visible tasks.
  [[nodiscard]] virtual std::optional<ListTasksRequest> ScopeTaskList(const ListTasksRequest& query,
                                                                      const RequestContext& context) const {
    (void)context;
    return query;
  }
};

class DispatchSecurity final {
 public:
  DispatchSecurity(std::shared_ptr<const SecurityPolicy> policy, TaskStore* task_store);
  [[nodiscard]] core::Result<void> Authorize(const DispatchRequest& request, RequestContext& context) const;
  [[nodiscard]] core::Result<void> AuthorizeResponse(const DispatchResponse& response,
                                                     const RequestContext& context) const;
  [[nodiscard]] core::Result<DispatchResponse> ListTasks(const ListTasksRequest& request,
                                                         const RequestContext& context) const;
  [[nodiscard]] core::Result<void> AuthorizeStreamEvent(const lf::a2a::v1::StreamResponse& event,
                                                        const RequestContext& context, std::string& task_id) const;
  [[nodiscard]] core::Result<void> ProtectStream(const DispatchRequest& request, const RequestContext& context,
                                                 DispatchResponse& response) const;

 private:
  struct VisibleTaskPage {
    std::vector<std::string> task_ids;
    std::size_t total_size = 0;
    std::string next_page_token;
  };
  [[nodiscard]] core::Result<VisibleTaskPage> SelectTaskIds(ListTasksRequest query, const RequestContext& context,
                                                            const std::optional<ListTaskCursor>& cursor,
                                                            std::size_t page_size) const;
  std::shared_ptr<const SecurityPolicy> policy_;
  TaskStore* task_store_;
};

}  // namespace a2a::server
