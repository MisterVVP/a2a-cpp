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
inline constexpr auto kDispatchCallbackException = "Server dispatch callback threw an exception";

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

template <typename T, typename Completion>
void CompleteDispatch(core::Result<T> result, const Completion& completion) {
  if (!result.ok()) {
    completion(result.error());
    return;
  }
  if constexpr (std::is_void_v<T>) {
    completion(DispatchResponse{});
  } else {
    completion(DispatchResponse(std::move(result.value())));
  }
}

template <typename Request, typename Response, typename Invoke, typename Completion>
void RoutePayload(const DispatchRequest& request, const char* mismatch, Invoke invoke, Completion completion) {
  const auto* payload = std::get_if<Request>(&request.payload);
  if (payload == nullptr) {
    completion(core::Error::Validation(mismatch));
    return;
  }
  invoke(*payload, [completion = std::move(completion)](core::Result<Response> result) {
    CompleteDispatch(std::move(result), completion);
  });
}

template <typename Executor, typename Context, typename Completion>
void RouteDispatch(Executor& executor, const DispatchRequest& request, Context& context, Completion completion) {
  using namespace core::protocol_error_messages;
  switch (request.operation) {
    case DispatcherOperation::kSendMessage:
      RoutePayload<lf::a2a::v1::SendMessageRequest, lf::a2a::v1::SendMessageResponse>(
          request, kDispatchPayloadTypeMismatchForSendMessage.data(),
          [&](const auto& payload, auto done) { executor.SendMessage(payload, context, std::move(done)); },
          std::move(completion));
      return;
    case DispatcherOperation::kSendStreamingMessage:
      RoutePayload<lf::a2a::v1::SendMessageRequest, std::unique_ptr<ServerStreamSession>>(
          request, kDispatchPayloadTypeMismatchForSendStreamingMessage.data(),
          [&](const auto& payload, auto done) {
            if constexpr (requires { executor.SendStreamingMessage(payload, context, std::move(done)); }) {
              executor.SendStreamingMessage(payload, context, std::move(done));
            } else {
              done(core::Error::Validation(kUnaryOnly));
            }
          },
          std::move(completion));
      return;
    case DispatcherOperation::kGetTask:
      RoutePayload<lf::a2a::v1::GetTaskRequest, lf::a2a::v1::Task>(
          request, kDispatchPayloadTypeMismatchForGetTask.data(),
          [&](const auto& payload, auto done) { executor.GetTask(payload, context, std::move(done)); },
          std::move(completion));
      return;
    case DispatcherOperation::kSubscribeTask:
      RoutePayload<lf::a2a::v1::GetTaskRequest, std::unique_ptr<ServerStreamSession>>(
          request, kDispatchPayloadTypeMismatchForGetTask.data(),
          [&](const auto& payload, auto done) {
            if constexpr (requires { executor.SubscribeTask(payload, context, std::move(done)); }) {
              executor.SubscribeTask(payload, context, std::move(done));
            } else {
              done(core::Error::Validation(kUnaryOnly));
            }
          },
          std::move(completion));
      return;
    case DispatcherOperation::kListTasks:
      RoutePayload<ListTasksRequest, ListTasksResponse>(
          request, kDispatchPayloadTypeMismatchForListTasks.data(),
          [&](const auto& payload, auto done) { executor.ListTasks(payload, context, std::move(done)); },
          std::move(completion));
      return;
    case DispatcherOperation::kCancelTask:
      RoutePayload<lf::a2a::v1::CancelTaskRequest, lf::a2a::v1::Task>(
          request, kDispatchPayloadTypeMismatchForCancelTask.data(),
          [&](const auto& payload, auto done) { executor.CancelTask(payload, context, std::move(done)); },
          std::move(completion));
      return;
    case DispatcherOperation::kCreateTaskPushNotificationConfig:
      RoutePayload<lf::a2a::v1::TaskPushNotificationConfig, lf::a2a::v1::TaskPushNotificationConfig>(
          request, kDispatchPayloadTypeMismatchForCreateTaskPushNotificationConfig.data(),
          [&](const auto& payload, auto done) {
            executor.CreateTaskPushNotificationConfig(payload, context, std::move(done));
          },
          std::move(completion));
      return;
    case DispatcherOperation::kGetTaskPushNotificationConfig:
      RoutePayload<lf::a2a::v1::GetTaskPushNotificationConfigRequest, lf::a2a::v1::TaskPushNotificationConfig>(
          request, kDispatchPayloadTypeMismatchForGetTaskPushNotificationConfig.data(),
          [&](const auto& payload, auto done) {
            executor.GetTaskPushNotificationConfig(payload, context, std::move(done));
          },
          std::move(completion));
      return;
    case DispatcherOperation::kListTaskPushNotificationConfigs:
      RoutePayload<lf::a2a::v1::ListTaskPushNotificationConfigsRequest,
                   lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
          request, kDispatchPayloadTypeMismatchForListTaskPushNotificationConfigs.data(),
          [&](const auto& payload, auto done) {
            executor.ListTaskPushNotificationConfigs(payload, context, std::move(done));
          },
          std::move(completion));
      return;
    case DispatcherOperation::kDeleteTaskPushNotificationConfig:
      RoutePayload<lf::a2a::v1::DeleteTaskPushNotificationConfigRequest, void>(
          request, kDispatchPayloadTypeMismatchForDeleteTaskPushNotificationConfig.data(),
          [&](const auto& payload, auto done) {
            executor.DeleteTaskPushNotificationConfig(payload, context, std::move(done));
          },
          std::move(completion));
      return;
    case DispatcherOperation::kGetExtendedAgentCard:
      break;  // Handled by the shared pipeline, before executor routing.
  }
  completion(core::Error::Validation(kUnsupportedDispatchOperation));
}
}  // namespace a2a::server::engine
