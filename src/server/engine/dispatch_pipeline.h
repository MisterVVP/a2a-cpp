// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/protocol_error_messages.h"
#include "a2a/server/dispatch_types.h"
#include "a2a/server/security_policy.h"
#include "a2a/server/server_interceptor.h"

namespace a2a::server::engine {
inline constexpr auto kUnaryOnly = "Async dispatcher supports unary operations only";
inline constexpr auto kUnsupportedDispatchOperation = "Unsupported dispatcher operation";
inline constexpr auto kExtendedCardPayloadMismatch = "Dispatch payload type mismatch for GetExtendedAgentCard";
inline constexpr auto kDispatchException = "Server dispatch operation threw an exception";

struct DispatchPipeline final {
  std::shared_ptr<const DispatchSecurity> security;
  std::vector<std::shared_ptr<ServerInterceptor>> interceptors;
  std::shared_ptr<core::AgentCardProvider> agent_card_provider;

  core::Result<void> Before(const DispatchRequest& request, RequestContext& context,
                            std::optional<std::string>& principal) const;
  std::optional<DispatchRequest> Prepare(const DispatchRequest& request) const;
  std::optional<core::Result<DispatchResponse>> Handle(const DispatchRequest& request, RequestContext& context) const;
  void Project(const DispatchRequest& request, RequestContext& context, const std::optional<std::string>& principal,
               core::Result<DispatchResponse>& result) const;
  void After(const DispatchRequest& request, RequestContext& context, const std::optional<std::string>& principal,
             const core::Result<DispatchResponse>& result, bool contain_exceptions = false) const;
};

template <typename T>
core::Result<DispatchResponse> ToDispatchResponse(core::Result<T> result) {
  if (!result.ok()) {
    return result.error();
  }
  if constexpr (std::is_void_v<T>) {
    return DispatchResponse{};
  } else if constexpr (std::is_same_v<T, DispatchResponse>) {
    return std::move(result.value());
  } else {
    return DispatchResponse(std::move(result.value()));
  }
}

struct SyncDispatchResult final {
  using ReturnType = core::Result<DispatchResponse>;
  template <typename T>
  ReturnType operator()(core::Result<T> result) const {
    return ToDispatchResponse(std::move(result));
  }
};

template <typename Request, typename Adapter, typename Invoke>
typename Adapter::ReturnType RoutePayload(const DispatchRequest& request, const char* mismatch, const Adapter& adapt,
                                          Invoke invoke) {
  const auto* payload = std::get_if<Request>(&request.payload);
  if (payload == nullptr) {
    return adapt(core::Result<DispatchResponse>(core::Error::Validation(mismatch)));
  }
  return invoke(*payload);
}

// Keep Executor dependent so MSVC discards unary-only branches before lookup.
template <typename Executor, typename Context, typename Adapter>
typename Adapter::ReturnType InvokeSendStreamingMessage(Executor& executor,
                                                        const lf::a2a::v1::SendMessageRequest& payload,
                                                        Context& context, const Adapter& adapt) {
  if constexpr (requires { executor.SendStreamingMessage(payload, context); }) {
    return adapt(executor.SendStreamingMessage(payload, context));
  } else {
    return adapt(core::Result<DispatchResponse>(core::Error::Validation(kUnaryOnly)));
  }
}

template <typename Executor, typename Context, typename Adapter>
typename Adapter::ReturnType InvokeSubscribeTask(Executor& executor, const lf::a2a::v1::GetTaskRequest& payload,
                                                 Context& context, const Adapter& adapt) {
  if constexpr (requires { executor.SubscribeTask(payload, context); }) {
    return adapt(executor.SubscribeTask(payload, context));
  } else {
    return adapt(core::Result<DispatchResponse>(core::Error::Validation(kUnaryOnly)));
  }
}

template <typename Executor, typename Context, typename Adapter>
typename Adapter::ReturnType RouteDispatch(Executor& executor, const DispatchRequest& request, Context& context,
                                           const Adapter& adapt) {
  using namespace core::protocol_error_messages;
  switch (request.operation) {
    case DispatcherOperation::kSendMessage:
      return RoutePayload<lf::a2a::v1::SendMessageRequest>(
          request, kDispatchPayloadTypeMismatchForSendMessage.data(), adapt,
          [&](const auto& payload) { return adapt(executor.SendMessage(payload, context)); });
    case DispatcherOperation::kSendStreamingMessage:
      return RoutePayload<lf::a2a::v1::SendMessageRequest>(
          request, kDispatchPayloadTypeMismatchForSendStreamingMessage.data(), adapt,
          [&](const auto& payload) { return InvokeSendStreamingMessage(executor, payload, context, adapt); });
    case DispatcherOperation::kGetTask:
      return RoutePayload<lf::a2a::v1::GetTaskRequest>(
          request, kDispatchPayloadTypeMismatchForGetTask.data(), adapt,
          [&](const auto& payload) { return adapt(executor.GetTask(payload, context)); });
    case DispatcherOperation::kSubscribeTask:
      return RoutePayload<lf::a2a::v1::GetTaskRequest>(
          request, kDispatchPayloadTypeMismatchForGetTask.data(), adapt,
          [&](const auto& payload) { return InvokeSubscribeTask(executor, payload, context, adapt); });
    case DispatcherOperation::kListTasks:
      return RoutePayload<ListTasksRequest>(
          request, kDispatchPayloadTypeMismatchForListTasks.data(), adapt,
          [&](const auto& payload) { return adapt(executor.ListTasks(payload, context)); });
    case DispatcherOperation::kCancelTask:
      return RoutePayload<lf::a2a::v1::CancelTaskRequest>(
          request, kDispatchPayloadTypeMismatchForCancelTask.data(), adapt,
          [&](const auto& payload) { return adapt(executor.CancelTask(payload, context)); });
    case DispatcherOperation::kCreateTaskPushNotificationConfig:
      return RoutePayload<lf::a2a::v1::TaskPushNotificationConfig>(
          request, kDispatchPayloadTypeMismatchForCreateTaskPushNotificationConfig.data(), adapt,
          [&](const auto& payload) { return adapt(executor.CreateTaskPushNotificationConfig(payload, context)); });
    case DispatcherOperation::kGetTaskPushNotificationConfig:
      return RoutePayload<lf::a2a::v1::GetTaskPushNotificationConfigRequest>(
          request, kDispatchPayloadTypeMismatchForGetTaskPushNotificationConfig.data(), adapt,
          [&](const auto& payload) { return adapt(executor.GetTaskPushNotificationConfig(payload, context)); });
    case DispatcherOperation::kListTaskPushNotificationConfigs:
      return RoutePayload<lf::a2a::v1::ListTaskPushNotificationConfigsRequest>(
          request, kDispatchPayloadTypeMismatchForListTaskPushNotificationConfigs.data(), adapt,
          [&](const auto& payload) { return adapt(executor.ListTaskPushNotificationConfigs(payload, context)); });
    case DispatcherOperation::kDeleteTaskPushNotificationConfig:
      return RoutePayload<lf::a2a::v1::DeleteTaskPushNotificationConfigRequest>(
          request, kDispatchPayloadTypeMismatchForDeleteTaskPushNotificationConfig.data(), adapt,
          [&](const auto& payload) { return adapt(executor.DeleteTaskPushNotificationConfig(payload, context)); });
    case DispatcherOperation::kGetExtendedAgentCard:
      break;
  }
  return adapt(core::Result<DispatchResponse>(core::Error::Validation(kUnsupportedDispatchOperation)));
}
}  // namespace a2a::server::engine
