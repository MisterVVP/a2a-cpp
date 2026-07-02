// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <memory>
#include <shared_mutex>
#include <vector>

#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/result.h"
#include "a2a/server/agent_executor.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/request_context.h"
#include "a2a/server/server_interceptor.h"

namespace a2a::server {

class Dispatcher final {
 public:
  explicit Dispatcher(AgentExecutor* executor);
  explicit Dispatcher(AgentExecutor* executor, std::shared_ptr<core::AgentCardProvider> agent_card_provider);
  explicit Dispatcher(AgentExecutor* executor, std::vector<std::shared_ptr<ServerInterceptor>> interceptors);
  Dispatcher(AgentExecutor* executor, std::vector<std::shared_ptr<ServerInterceptor>> interceptors,
             std::shared_ptr<core::AgentCardProvider> agent_card_provider);

  [[nodiscard]] core::Result<DispatchResponse> Dispatch(const DispatchRequest& request, RequestContext& context) const;
  void AddInterceptor(std::shared_ptr<ServerInterceptor> interceptor);

 private:
  void RunAfterInterceptors(const DispatchRequest& request, RequestContext& context,
                            const core::Result<DispatchResponse>& result) const;

  AgentExecutor* executor_ = nullptr;
  std::shared_ptr<core::AgentCardProvider> agent_card_provider_;
  mutable std::shared_mutex interceptor_mutex_;
  std::vector<std::shared_ptr<ServerInterceptor>> interceptors_;
};

}  // namespace a2a::server
