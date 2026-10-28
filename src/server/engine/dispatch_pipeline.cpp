// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "server/engine/dispatch_pipeline.h"

#include <ranges>

#include "a2a/core/protocol_errors.h"
#include "a2a/server/tasks/task_history.h"
namespace a2a::server::engine {
namespace {
void RestorePrincipal(RequestContext& context, const std::optional<std::string>& principal) {
  if (principal.has_value()) {
    context.principal = *principal;
  }
}
core::AgentCardRequestContext ToAgentCardRequestContext(const RequestContext& context, std::string_view tenant) {
  return {.tenant = tenant.empty() ? std::optional<std::string>{} : std::optional<std::string>(tenant),
          .remote_address = context.remote_address,
          .client_headers = context.client_headers,
          .auth_metadata = context.auth_metadata};
}

void ProjectTaskResponse(const DispatchRequest& request, DispatchResponse& response) {
  if (request.operation == DispatcherOperation::kGetTask) {
    const auto& query = std::get<lf::a2a::v1::GetTaskRequest>(request.payload);
    if (query.has_history_length()) {
      ApplyHistoryRetention(&std::get<lf::a2a::v1::Task>(response.payload()),
                            query.history_length() <= 0 ? 0 : static_cast<std::size_t>(query.history_length()));
    }
  } else if (request.operation == DispatcherOperation::kSendMessage) {
    const auto& query = std::get<lf::a2a::v1::SendMessageRequest>(request.payload);
    auto& send = std::get<lf::a2a::v1::SendMessageResponse>(response.payload());
    if (query.configuration().has_history_length() && send.has_task()) {
      const auto limit = query.configuration().history_length();
      ApplyHistoryRetention(send.mutable_task(), limit <= 0 ? 0 : static_cast<std::size_t>(limit));
    }
  }
}

core::Result<DispatchResponse> DispatchExtendedAgentCard(
    const DispatchRequest& request, RequestContext& context,
    const std::shared_ptr<core::AgentCardProvider>& agent_card_provider) {
  const auto* payload = std::get_if<lf::a2a::v1::GetExtendedAgentCardRequest>(&request.payload);
  if (payload == nullptr) {
    return core::Error::Validation(kExtendedCardPayloadMismatch);
  }
  if (agent_card_provider == nullptr) {
    return core::protocol_errors::ExtendedAgentCardNotConfigured();
  }
  auto response = agent_card_provider->GetExtendedAgentCard(ToAgentCardRequestContext(context, payload->tenant()));
  if (!response.ok()) {
    return response.error();
  }
  return DispatchResponse(std::move(response.value()));
}

}  // namespace

core::Result<void> DispatchPipeline::Before(const DispatchRequest& request, RequestContext& context,
                                            std::optional<std::string>& principal) const {
  const auto authorized = security != nullptr ? security->Authorize(request, context) : core::Result<void>{};
  principal = security != nullptr ? std::optional(context.principal) : std::nullopt;
  if (!authorized.ok()) {
    return authorized.error();
  }
  for (const auto& interceptor : interceptors) {
    if (interceptor == nullptr) {
      continue;
    }
    const auto result = interceptor->BeforeDispatch(request, context);
    RestorePrincipal(context, principal);
    if (!result.ok()) {
      return result.error();
    }
  }
  return {};
}

std::optional<DispatchRequest> DispatchPipeline::Prepare(const DispatchRequest& request) const {
  if (security == nullptr) {
    return std::nullopt;
  }
  const auto* get = std::get_if<lf::a2a::v1::GetTaskRequest>(&request.payload);
  const auto* send = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
  const bool get_history =
      request.operation == DispatcherOperation::kGetTask && get != nullptr && get->has_history_length();
  const bool send_history = request.operation == DispatcherOperation::kSendMessage && send != nullptr &&
                            send->configuration().has_history_length();
  if (!get_history && !send_history) {
    return std::nullopt;
  }
  auto prepared = request;
  if (request.operation == DispatcherOperation::kGetTask) {
    if (auto* query = std::get_if<lf::a2a::v1::GetTaskRequest>(&prepared.payload)) {
      query->clear_history_length();
    }
  } else if (request.operation == DispatcherOperation::kSendMessage) {
    if (auto* query = std::get_if<lf::a2a::v1::SendMessageRequest>(&prepared.payload)) {
      if (query->configuration().has_history_length()) {
        query->mutable_configuration()->clear_history_length();
      }
    }
  }
  return prepared;
}

std::optional<core::Result<DispatchResponse>> DispatchPipeline::Handle(const DispatchRequest& request,
                                                                       RequestContext& context) const {
  const auto* list = std::get_if<ListTasksRequest>(&request.payload);
  if (security != nullptr && request.operation == DispatcherOperation::kListTasks && list != nullptr) {
    return security->ListTasks(*list, context);
  }
  if (request.operation == DispatcherOperation::kGetExtendedAgentCard) {
    return DispatchExtendedAgentCard(request, context, agent_card_provider);
  }
  return std::nullopt;
}

void DispatchPipeline::Project(const DispatchRequest& request, RequestContext& context,
                               const std::optional<std::string>& principal,
                               core::Result<DispatchResponse>& result) const {
  RestorePrincipal(context, principal);
  if (security != nullptr && result.ok()) {
    auto authorized = security->AuthorizeResponse(result.value(), context);
    if (authorized.ok()) {
      authorized = security->ProtectStream(request, context, result.value());
    }
    if (!authorized.ok()) {
      result = authorized.error();
    }
  }
  if (result.ok()) {
    ProjectTaskResponse(request, result.value());
  }
}

void DispatchPipeline::After(const DispatchRequest& request, RequestContext& context,
                             const std::optional<std::string>& principal, const core::Result<DispatchResponse>& result,
                             bool contain_exceptions) const {
  RestorePrincipal(context, principal);
  for (const auto& interceptor : std::ranges::reverse_view(interceptors)) {
    if (interceptor == nullptr) {
      continue;
    }
    if (!contain_exceptions) {
      interceptor->AfterDispatch(request, context, result);
      RestorePrincipal(context, principal);
      continue;
    }
    try {
      interceptor->AfterDispatch(request, context, result);
    } catch (...) {
      // Restore authentication even when an audit hook throws, then run the rest.
      RestorePrincipal(context, principal);
      continue;
    }
    RestorePrincipal(context, principal);
  }
}
}  // namespace a2a::server::engine
