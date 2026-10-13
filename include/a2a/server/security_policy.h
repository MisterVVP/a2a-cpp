// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <string>

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
};

class DispatchSecurity final {
 public:
  DispatchSecurity(std::shared_ptr<const SecurityPolicy> policy, TaskStore* task_store);
  [[nodiscard]] core::Result<void> Authorize(const DispatchRequest& request, RequestContext& context) const;
  [[nodiscard]] core::Result<DispatchResponse> ListTasks(const ListTasksRequest& request,
                                                         const RequestContext& context) const;
  void ProtectStream(const DispatchRequest& request, const RequestContext& context, DispatchResponse& response) const;

 private:
  std::shared_ptr<const SecurityPolicy> policy_;
  TaskStore* task_store_;
};

}  // namespace a2a::server
