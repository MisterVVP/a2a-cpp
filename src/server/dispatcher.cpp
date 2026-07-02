// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/dispatcher.h"

#include <array>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_error_messages.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/server/tasks/task_history.h"

namespace a2a::server {
namespace {

template <std::size_t MessageSize>
[[nodiscard]] core::Error DispatchPayloadTypeMismatchError(const std::array<char, MessageSize>& message) {
  return core::Error::Validation(core::protocol_error_messages::ToString(message));
}

bool IsPushNotificationOperation(DispatcherOperation operation) {
  return operation == DispatcherOperation::kCreateTaskPushNotificationConfig ||
         operation == DispatcherOperation::kGetTaskPushNotificationConfig ||
         operation == DispatcherOperation::kListTaskPushNotificationConfigs ||
         operation == DispatcherOperation::kDeleteTaskPushNotificationConfig;
}

core::AgentCardRequestContext ToAgentCardRequestContext(const RequestContext& context, std::string_view tenant) {
  return {.tenant = tenant.empty() ? std::optional<std::string>{} : std::optional<std::string>(tenant),
          .remote_address = context.remote_address,
          .client_headers = context.client_headers,
          .auth_metadata = context.auth_metadata};
}

core::Result<DispatchResponse> DispatchPushToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                      RequestContext& context) {
  switch (request.operation) {
    case DispatcherOperation::kCreateTaskPushNotificationConfig: {
      const auto* payload = std::get_if<lf::a2a::v1::TaskPushNotificationConfig>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForCreateTaskPushNotificationConfig);
      }
      const auto response = executor.CreateTaskPushNotificationConfig(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kGetTaskPushNotificationConfig: {
      const auto* payload = std::get_if<lf::a2a::v1::GetTaskPushNotificationConfigRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForGetTaskPushNotificationConfig);
      }
      const auto response = executor.GetTaskPushNotificationConfig(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kListTaskPushNotificationConfigs: {
      const auto* payload = std::get_if<lf::a2a::v1::ListTaskPushNotificationConfigsRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForListTaskPushNotificationConfigs);
      }
      const auto response = executor.ListTaskPushNotificationConfigs(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse(response.value());
    }
    case DispatcherOperation::kDeleteTaskPushNotificationConfig: {
      const auto* payload = std::get_if<lf::a2a::v1::DeleteTaskPushNotificationConfigRequest>(&request.payload);
      if (payload == nullptr) {
        return DispatchPayloadTypeMismatchError(
            core::protocol_error_messages::kDispatchPayloadTypeMismatchForDeleteTaskPushNotificationConfig);
      }
      const auto response = executor.DeleteTaskPushNotificationConfig(*payload, context);
      if (!response.ok()) {
        return response.error();
      }
      return DispatchResponse();
    }
    case DispatcherOperation::kSendMessage:
    case DispatcherOperation::kSendStreamingMessage:
    case DispatcherOperation::kGetTask:
    case DispatcherOperation::kSubscribeTask:
    case DispatcherOperation::kListTasks:
    case DispatcherOperation::kCancelTask:
    case DispatcherOperation::kGetExtendedAgentCard:
      return core::Error::Validation("Dispatch operation is not a push notification operation");
  }
  return core::Error::Validation("Unsupported push notification dispatcher operation");
}

core::Result<DispatchResponse> DispatchSubscribeToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                           RequestContext& context) {
  const auto* payload = std::get_if<lf::a2a::v1::GetTaskRequest>(&request.payload);
  if (payload == nullptr) {
    return DispatchPayloadTypeMismatchError(core::protocol_error_messages::kDispatchPayloadTypeMismatchForGetTask);
  }
  auto response = executor.SubscribeTask(*payload, context);
  if (!response.ok()) {
    return response.error();
  }
  return DispatchResponse(std::move(response.value()));
}

core::Result<DispatchResponse> DispatchSendMessageToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                             RequestContext& context) {
  const auto* payload = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
  if (payload == nullptr) {
    return DispatchPayloadTypeMismatchError(core::protocol_error_messages::kDispatchPayloadTypeMismatchForSendMessage);
  }
  const auto response = executor.SendMessage(*payload, context);
  if (!response.ok()) {
    return response.error();
  }
  return DispatchResponse(response.value());
}

core::Result<DispatchResponse> DispatchSendStreamingMessageToExecutor(AgentExecutor& executor,
                                                                      const DispatchRequest& request,
                                                                      RequestContext& context) {
  const auto* payload = std::get_if<lf::a2a::v1::SendMessageRequest>(&request.payload);
  if (payload == nullptr) {
    return DispatchPayloadTypeMismatchError(
        core::protocol_error_messages::kDispatchPayloadTypeMismatchForSendStreamingMessage);
  }
  auto response = executor.SendStreamingMessage(*payload, context);
  if (!response.ok()) {
    return response.error();
  }
  return DispatchResponse(std::move(response.value()));
}

core::Result<DispatchResponse> DispatchGetTaskToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                         RequestContext& context) {
  const auto* payload = std::get_if<lf::a2a::v1::GetTaskRequest>(&request.payload);
  if (payload == nullptr) {
    return DispatchPayloadTypeMismatchError(core::protocol_error_messages::kDispatchPayloadTypeMismatchForGetTask);
  }
  auto response = executor.GetTask(*payload, context);
  if (!response.ok()) {
    return response.error();
  }
  lf::a2a::v1::Task task = std::move(response.value());
  if (payload->has_history_length()) {
    ApplyHistoryRetention(&task, static_cast<std::size_t>(payload->history_length()));
  }
  return DispatchResponse(std::move(task));
}

core::Result<DispatchResponse> DispatchListTasksToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                           RequestContext& context) {
  const auto* payload = std::get_if<ListTasksRequest>(&request.payload);
  if (payload == nullptr) {
    return DispatchPayloadTypeMismatchError(core::protocol_error_messages::kDispatchPayloadTypeMismatchForListTasks);
  }
  const auto response = executor.ListTasks(*payload, context);
  if (!response.ok()) {
    return response.error();
  }
  return DispatchResponse(response.value());
}

core::Result<DispatchResponse> DispatchCancelTaskToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                            RequestContext& context) {
  const auto* payload = std::get_if<lf::a2a::v1::CancelTaskRequest>(&request.payload);
  if (payload == nullptr) {
    return DispatchPayloadTypeMismatchError(core::protocol_error_messages::kDispatchPayloadTypeMismatchForCancelTask);
  }
  const auto response = executor.CancelTask(*payload, context);
  if (!response.ok()) {
    return response.error();
  }
  return DispatchResponse(response.value());
}

core::Result<DispatchResponse> DispatchExtendedAgentCard(
    const DispatchRequest& request, RequestContext& context,
    const std::shared_ptr<core::AgentCardProvider>& agent_card_provider) {
  const auto* payload = std::get_if<lf::a2a::v1::GetExtendedAgentCardRequest>(&request.payload);
  if (payload == nullptr) {
    return core::Error::Validation("Dispatch payload type mismatch for GetExtendedAgentCard");
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

core::Result<DispatchResponse> DispatchToExecutor(AgentExecutor& executor, const DispatchRequest& request,
                                                  RequestContext& context,
                                                  const std::shared_ptr<core::AgentCardProvider>& agent_card_provider) {
  if (IsPushNotificationOperation(request.operation)) {
    return DispatchPushToExecutor(executor, request, context);
  }

  switch (request.operation) {
    case DispatcherOperation::kSendMessage:
      return DispatchSendMessageToExecutor(executor, request, context);
    case DispatcherOperation::kSendStreamingMessage:
      return DispatchSendStreamingMessageToExecutor(executor, request, context);
    case DispatcherOperation::kGetTask:
      return DispatchGetTaskToExecutor(executor, request, context);
    case DispatcherOperation::kSubscribeTask: {
      return DispatchSubscribeToExecutor(executor, request, context);
    }
    case DispatcherOperation::kListTasks:
      return DispatchListTasksToExecutor(executor, request, context);
    case DispatcherOperation::kCancelTask:
      return DispatchCancelTaskToExecutor(executor, request, context);
    case DispatcherOperation::kCreateTaskPushNotificationConfig:
    case DispatcherOperation::kGetTaskPushNotificationConfig:
    case DispatcherOperation::kListTaskPushNotificationConfigs:
    case DispatcherOperation::kDeleteTaskPushNotificationConfig:
      return core::Error::Validation("Push notification dispatch was not handled by push dispatcher");
    case DispatcherOperation::kGetExtendedAgentCard:
      return DispatchExtendedAgentCard(request, context, agent_card_provider);
  }

  return core::Error::Validation("Unsupported dispatcher operation");
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
    return core::Error::Internal("Server dispatcher executor is not configured");
  }

  std::shared_lock<std::shared_mutex> read_lock(interceptor_mutex_);
  for (const auto& interceptor : interceptors_) {
    if (interceptor == nullptr) {
      continue;
    }
    const auto before_result = interceptor->BeforeDispatch(request, context);
    if (!before_result.ok()) {
      core::Result<DispatchResponse> failure = before_result.error();
      read_lock.unlock();
      RunAfterInterceptors(request, context, failure);
      return before_result.error();
    }
  }
  read_lock.unlock();

  auto dispatch_result = DispatchToExecutor(*executor_, request, context, agent_card_provider_);
  RunAfterInterceptors(request, context, dispatch_result);
  return dispatch_result;
}

void Dispatcher::AddInterceptor(std::shared_ptr<ServerInterceptor> interceptor) {
  if (interceptor == nullptr) {
    return;
  }
  std::unique_lock<std::shared_mutex> lock(interceptor_mutex_);
  interceptors_.push_back(std::move(interceptor));
}

void Dispatcher::RunAfterInterceptors(const DispatchRequest& request, RequestContext& context,
                                      const core::Result<DispatchResponse>& result) const {
  std::shared_lock<std::shared_mutex> read_lock(interceptor_mutex_);
  for (const auto& interceptor : std::ranges::reverse_view(interceptors_)) {
    if (interceptor == nullptr) {
      continue;
    }
    interceptor->AfterDispatch(request, context, result);
  }
}

}  // namespace a2a::server
