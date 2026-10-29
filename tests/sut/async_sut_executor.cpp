// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "sut/async_sut_executor.h"

namespace a2a::tests::sut {
AsyncSutExecutor::AsyncSutExecutor(std::shared_ptr<SutApplication> application)
    : application_(std::move(application)) {}
void AsyncSutExecutor::SendMessage(lf::a2a::v1::SendMessageRequest request,
                                   server::engine::RequestExecutionContext context,
                                   Completion<lf::a2a::v1::SendMessageResponse> complete) {
  Submit(std::move(request), std::move(context), std::move(complete), &server::AgentExecutor::SendMessage);
}
void AsyncSutExecutor::GetTask(lf::a2a::v1::GetTaskRequest request, server::engine::RequestExecutionContext context,
                               Completion<lf::a2a::v1::Task> complete) {
  Submit(std::move(request), std::move(context), std::move(complete), &server::AgentExecutor::GetTask);
}
void AsyncSutExecutor::ListTasks(server::ListTasksRequest request, server::engine::RequestExecutionContext context,
                                 Completion<server::ListTasksResponse> complete) {
  Submit(std::move(request), std::move(context), std::move(complete), &server::AgentExecutor::ListTasks);
}
void AsyncSutExecutor::CancelTask(lf::a2a::v1::CancelTaskRequest request,
                                  server::engine::RequestExecutionContext context,
                                  Completion<lf::a2a::v1::Task> complete) {
  Submit(std::move(request), std::move(context), std::move(complete), &server::AgentExecutor::CancelTask);
}
void AsyncSutExecutor::CreateTaskPushNotificationConfig(lf::a2a::v1::TaskPushNotificationConfig request,
                                                        server::engine::RequestExecutionContext context,
                                                        Completion<lf::a2a::v1::TaskPushNotificationConfig> complete) {
  Submit(std::move(request), std::move(context), std::move(complete),
         &server::AgentExecutor::CreateTaskPushNotificationConfig);
}
void AsyncSutExecutor::GetTaskPushNotificationConfig(lf::a2a::v1::GetTaskPushNotificationConfigRequest request,
                                                     server::engine::RequestExecutionContext context,
                                                     Completion<lf::a2a::v1::TaskPushNotificationConfig> complete) {
  Submit(std::move(request), std::move(context), std::move(complete),
         &server::AgentExecutor::GetTaskPushNotificationConfig);
}
void AsyncSutExecutor::ListTaskPushNotificationConfigs(
    lf::a2a::v1::ListTaskPushNotificationConfigsRequest request, server::engine::RequestExecutionContext context,
    Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> complete) {
  Submit(std::move(request), std::move(context), std::move(complete),
         &server::AgentExecutor::ListTaskPushNotificationConfigs);
}
void AsyncSutExecutor::DeleteTaskPushNotificationConfig(lf::a2a::v1::DeleteTaskPushNotificationConfigRequest request,
                                                        server::engine::RequestExecutionContext context,
                                                        Completion<void> complete) {
  Submit(std::move(request), std::move(context), std::move(complete),
         &server::AgentExecutor::DeleteTaskPushNotificationConfig);
}
}  // namespace a2a::tests::sut
