// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once
#include <type_traits>
#include <utility>

#include "a2a/core/agent_card/agent_card_provider.h"
#include "a2a/core/version.h"
#include "a2a/server/dispatcher.h"
#include "a2a/server/engine/async_agent_executor.h"
namespace a2a::tests::sut {
// TCK fixture only: existing synchronous semantics execute on the async
// dispatcher's bounded workers. This adapter is not non-blocking backend I/O.
class BlockingSutExecutor final : public server::engine::AsyncAgentExecutor, public core::AgentCardProvider {
 public:
  explicit BlockingSutExecutor(server::Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
  void SendMessage(lf::a2a::v1::SendMessageRequest request, server::engine::RequestExecutionContext context,
                   Completion<lf::a2a::v1::SendMessageResponse> complete) override {
    auto metadata = context.Metadata();
    complete(Invoke<lf::a2a::v1::SendMessageResponse>(
        {.operation = server::DispatcherOperation::kSendMessage, .payload = std::move(request)}, metadata));
  }
  void GetTask(lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context,
               Completion<lf::a2a::v1::Task> complete) override {
    auto metadata = context.Metadata();
    complete(Invoke<lf::a2a::v1::Task>(
        {.operation = server::DispatcherOperation::kGetTask, .payload = std::move(request)}, metadata));
  }
  void ListTasks(server::ListTasksRequest request, server::engine::RequestExecutionContext context,
                 Completion<server::ListTasksResponse> complete) override {
    auto metadata = context.Metadata();
    complete(Invoke<server::ListTasksResponse>(
        {.operation = server::DispatcherOperation::kListTasks, .payload = std::move(request)}, metadata));
  }
  void CancelTask(lf::a2a::v1::CancelTaskRequest request, server::engine::RequestExecutionContext context,
                  Completion<lf::a2a::v1::Task> complete) override {
    auto metadata = context.Metadata();
    complete(Invoke<lf::a2a::v1::Task>(
        {.operation = server::DispatcherOperation::kCancelTask, .payload = std::move(request)}, metadata));
  }
  void CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request,
                                        server::engine::RequestExecutionContext context,
                                        Completion<lf::a2a::v1::TaskPushNotificationConfig> complete) override {
    auto metadata = context.Metadata();
    complete(Invoke<lf::a2a::v1::TaskPushNotificationConfig>(
        {.operation = server::DispatcherOperation::kCreateTaskPushNotificationConfig, .payload = std::move(request)},
        metadata));
  }
  void GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                     server::engine::RequestExecutionContext context,
                                     Completion<lf::a2a::v1::TaskPushNotificationConfig> complete) override {
    auto metadata = context.Metadata();
    complete(Invoke<lf::a2a::v1::TaskPushNotificationConfig>(
        {.operation = server::DispatcherOperation::kGetTaskPushNotificationConfig, .payload = std::move(request)},
        metadata));
  }
  void ListTaskPushNotificationConfigs(
      lf::a2a::v1::ListTaskPushNotificationConfigsRequest request, server::engine::RequestExecutionContext context,
      Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> complete) override {
    auto metadata = context.Metadata();
    complete(Invoke<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
        {.operation = server::DispatcherOperation::kListTaskPushNotificationConfigs, .payload = std::move(request)},
        metadata));
  }
  void DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
                                        server::engine::RequestExecutionContext context,
                                        Completion<void> complete) override {
    auto metadata = context.Metadata();
    complete(Invoke<void>(
        {.operation = server::DispatcherOperation::kDeleteTaskPushNotificationConfig, .payload = std::move(request)},
        metadata));
  }
  [[nodiscard]] core::Result<lf::a2a::v1::AgentCard> GetExtendedAgentCard(
      const core::AgentCardRequestContext& context) const override {
    server::RequestContext metadata;
    metadata.remote_address = context.remote_address;
    metadata.client_headers = context.client_headers;
    metadata.auth_metadata = context.auth_metadata;
    metadata.protocol_version = core::Version::kProtocolVersion;
    lf::a2a::v1::GetExtendedAgentCardRequest request;
    if (context.tenant.has_value()) {
      request.set_tenant(*context.tenant);
    }
    return Invoke<lf::a2a::v1::AgentCard>(
        {.operation = server::DispatcherOperation::kGetExtendedAgentCard, .payload = std::move(request)}, metadata);
  }

 private:
  template <typename T>
  core::Result<T> Invoke(const server::DispatchRequest& request, server::RequestContext& context) const {
    auto result = dispatcher_.Dispatch(request, context);
    if (!result.ok()) {
      return result.error();
    }
    if constexpr (std::is_void_v<T>) {
      return {};
    } else {
      if (auto* payload = std::get_if<T>(&result.value().payload())) {
        return std::move(*payload);
      }
      return core::Error::Internal(kUnexpectedPayload);
    }
  }
  static constexpr auto kUnexpectedPayload = "Unexpected blocking SUT dispatch payload";
  server::Dispatcher& dispatcher_;
};
}  // namespace a2a::tests::sut
