// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/dispatcher.h"

#include <mutex>
#include <utility>

#include "server/engine/dispatch_pipeline.h"
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
#include "core/subscription_diagnostics.h"
#endif
namespace a2a::server {
namespace {
constexpr auto kExecutorNotConfigured = "Server dispatcher executor is not configured";
core::Result<DispatchResponse> RouteSync(AgentExecutor& executor, const DispatchRequest& request,
                                         RequestContext& context) {
#if defined(A2A_ENABLE_SUBSCRIPTION_DIAGNOSTICS)
  if (request.operation == DispatcherOperation::kCancelTask &&
      std::holds_alternative<lf::a2a::v1::CancelTaskRequest>(request.payload)) {
    const core::subscription_diagnostics::ScopedTimer timer(
        core::subscription_diagnostics::Phase::kServerCancelTaskTotal);
    return engine::RouteDispatch(executor, request, context, engine::SyncDispatchResult{});
  }
#endif
  return engine::RouteDispatch(executor, request, context, engine::SyncDispatchResult{});
}
}  // namespace
Dispatcher::Dispatcher(AgentExecutor* executor) : executor_(executor) {}

Dispatcher::Dispatcher(AgentExecutor* executor, std::shared_ptr<core::AgentCardProvider> agent_card_provider)
    : executor_(executor), agent_card_provider_(std::move(agent_card_provider)) {}

Dispatcher::Dispatcher(AgentExecutor* executor, std::vector<std::shared_ptr<ServerInterceptor>> interceptors)
    : Dispatcher(executor, std::move(interceptors), nullptr) {}

Dispatcher::Dispatcher(AgentExecutor* executor, std::vector<std::shared_ptr<ServerInterceptor>> interceptors,
                       std::shared_ptr<core::AgentCardProvider> agent_card_provider)
    : executor_(executor),
      agent_card_provider_(std::move(agent_card_provider)),
      interceptors_(std::move(interceptors)) {}

core::Result<DispatchResponse> Dispatcher::Dispatch(const DispatchRequest& request, RequestContext& context) const {
  if (executor_ == nullptr) {
    return core::Error::Internal(kExecutorNotConfigured);
  }
  engine::DispatchPipeline pipeline;
  {
    std::shared_lock lock(interceptor_mutex_);
    pipeline = {.security = security_, .interceptors = interceptors_, .agent_card_provider = agent_card_provider_};
  }
  std::optional<std::string> principal;
  const auto before = pipeline.Before(request, context, principal);
  if (!before.ok()) {
    core::Result<DispatchResponse> failure = before.error();
    pipeline.After(request, context, principal, failure);
    return failure;
  }
  auto result = pipeline.Handle(request, context);
  if (!result.has_value()) {
    const auto prepared = pipeline.Prepare(request);
    result = RouteSync(*executor_, prepared.has_value() ? *prepared : request, context);
  }
  if (!result.has_value()) {
    return core::Error::Internal(engine::kUnsupportedDispatchOperation);
  }
  pipeline.Project(request, context, principal, *result);
  pipeline.After(request, context, principal, *result);
  return std::move(*result);
}
void Dispatcher::SetSecurityPolicy(std::shared_ptr<const SecurityPolicy> policy, TaskStore* task_store) {
  std::unique_lock lock(interceptor_mutex_);
  security_ = std::make_shared<DispatchSecurity>(std::move(policy), task_store);
}

void Dispatcher::AddInterceptor(std::shared_ptr<ServerInterceptor> interceptor) {
  if (interceptor == nullptr) {
    return;
  }
  std::unique_lock<std::shared_mutex> lock(interceptor_mutex_);
  interceptors_.push_back(std::move(interceptor));
}

}  // namespace a2a::server
