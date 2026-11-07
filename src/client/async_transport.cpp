// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/async_transport.h"

#include <chrono>
#include <utility>
namespace a2a::client {
namespace {
constexpr const char* kUnsupportedAsync = "transport does not implement async unary calls";
constexpr const char* kStartFailed = "async transport initiation threw an exception";
template <typename T, typename Initiate>
core::Result<core::AsyncHandle> StartTransportCall(core::AsyncScope& scope, core::Completion<T> callback,
                                                   std::chrono::milliseconds timeout, const Initiate& initiate) {
  auto started = scope.Start<T>(std::move(callback), timeout);
  if (!started.ok()) {
    return started.error();
  }
  auto token = std::move(started.value());
  try {
    if (token.IsActive()) {
      initiate(token);
    }
  } catch (...) {
    token.Complete(core::Error::Internal(kStartFailed));
  }
  return token.handle();
}
}  // namespace
core::Result<core::AsyncHandle> AsyncClientTransport::SendMessage(
    const lf::a2a::v1::SendMessageRequest& request, core::Completion<lf::a2a::v1::SendMessageResponse> completion,
    const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::SendMessageResponse>(
      async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& token) {
        StartSendMessage(request, options, token);
      });
}
void AsyncClientTransport::StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
                                            const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> AsyncClientTransport::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                              core::Completion<lf::a2a::v1::Task> completion,
                                                              const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::Task>(
      async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::Task>& token) { StartGetTask(request, options, token); });
}
void AsyncClientTransport::StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                                        const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> AsyncClientTransport::ListTasks(const ListTasksRequest& request,
                                                                core::Completion<ListTasksResponse> completion,
                                                                const CallOptions& options) {
  return StartTransportCall<ListTasksResponse>(
      async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<ListTasksResponse>& token) { StartListTasks(request, options, token); });
}
void AsyncClientTransport::StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                                          const core::AsyncCompletion<ListTasksResponse>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> AsyncClientTransport::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                                 core::Completion<lf::a2a::v1::Task> completion,
                                                                 const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::Task>(
      async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::Task>& token) { StartCancelTask(request, options, token); });
}
void AsyncClientTransport::StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                                           const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> AsyncClientTransport::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request,
    core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::TaskPushNotificationConfig>(
      async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& token) {
        StartCreateTaskPushNotificationConfig(request, options, token);
      });
}
void AsyncClientTransport::StartCreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> AsyncClientTransport::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
    core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::TaskPushNotificationConfig>(
      async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& token) {
        StartGetTaskPushNotificationConfig(request, options, token);
      });
}
void AsyncClientTransport::StartGetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> AsyncClientTransport::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
    core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion, const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
      async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& token) {
        StartListTaskPushNotificationConfigs(request, options, token);
      });
}
void AsyncClientTransport::StartListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> AsyncClientTransport::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, core::Completion<void> completion,
    const CallOptions& options) {
  return StartTransportCall<void>(async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
                                  [&](const core::AsyncCompletion<void>& token) {
                                    StartDeleteTaskPushNotificationConfig(request, options, token);
                                  });
}
void AsyncClientTransport::StartDeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<void>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
}  // namespace a2a::client
