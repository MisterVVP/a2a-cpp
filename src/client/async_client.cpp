// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <chrono>
#include <ranges>
#include <utility>

#include "a2a/client/client.h"
#include "a2a/core/protocol_methods.h"

namespace a2a::client {
namespace {
constexpr const char* kUnsupportedAsync = "transport does not implement async unary calls";
constexpr const char* kMissingCompletion = "async completion callback is required";
constexpr const char* kMissingTransport = "Client transport is not configured";
constexpr const char* kStartFailed = "async transport initiation threw an exception";
using Interceptors = std::vector<std::shared_ptr<ClientInterceptor>>;

void NotifyAfter(const Interceptors& interceptors, const ClientCallContext& context, const ClientCallResult& result) {
  for (const auto& interceptor : std::ranges::reverse_view(interceptors)) {
    try {
      interceptor->AfterCall(context, result);
    } catch (...) {
      continue;
    }
  }
}

template <typename T>
core::Completion<T> InterceptCompletion(std::string_view operation, CallOptions options, Interceptors interceptors,
                                        core::Completion<T> callback) {
  // Snapshot ownership lets callbacks and AfterCall safely outlive the client.
  return [operation, options = std::move(options), interceptors = std::move(interceptors),
          callback = std::move(callback)](core::Result<T> result) mutable {
    const ClientCallContext context{.operation = operation, .options = &options};
    NotifyAfter(interceptors, context,
                result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
    callback(std::move(result));
  };
}

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

template <typename T, typename Initiate>
core::Result<core::AsyncHandle> StartClientCall(const std::shared_ptr<ClientTransport>& transport,
                                                std::string_view operation, const CallOptions& options,
                                                const Interceptors& interceptors, core::Completion<T> callback,
                                                const Initiate& initiate) {
  if (!transport) {
    return core::Error::Internal(kMissingTransport);
  }
  if (!callback) {
    return core::Error::Validation(kMissingCompletion);
  }
  const ClientCallContext context{.operation = operation, .options = &options};
  for (const auto& interceptor : interceptors) {
    try {
      interceptor->BeforeCall(context);
    } catch (...) {
      continue;
    }
  }
  if (!interceptors.empty()) {
    callback = InterceptCompletion<T>(operation, options, interceptors, std::move(callback));
  }
  auto started = initiate(std::move(callback));
  if (!started.ok()) {
    NotifyAfter(interceptors, context, ClientCallResult{.ok = false, .error = started.error()});
  }
  return started;
}
}  // namespace

A2AClient::~A2AClient() {
  try {
    (void)Destroy();
  } catch (...) {
    // A custom shutdown hook must not throw from client destruction. Releasing
    // the transport still closes its base completion scope and owned resources.
    closing_transport_.reset();
  }
}

core::Result<core::AsyncHandle> ClientTransport::SendMessageAsync(
    const lf::a2a::v1::SendMessageRequest& request, core::Completion<lf::a2a::v1::SendMessageResponse> completion,
    const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::SendMessageResponse>(
      *async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& token) {
        StartSendMessage(request, options, token);
      });
}
void ClientTransport::StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
                                       const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> A2AClient::SendMessageAsync(
    const lf::a2a::v1::SendMessageRequest& request, core::Completion<lf::a2a::v1::SendMessageResponse> completion,
    const CallOptions& options) {
  auto transport = TransportSnapshot();
  Interceptors interceptors;
  {
    std::lock_guard lock(interceptor_mutex_);
    interceptors = interceptors_;
  }
  return StartClientCall<lf::a2a::v1::SendMessageResponse>(
      transport, core::protocol_methods::kSendMessage, options, interceptors, std::move(completion),
      [&](core::Completion<lf::a2a::v1::SendMessageResponse> callback) {
        return transport->SendMessageAsync(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> ClientTransport::GetTaskAsync(const lf::a2a::v1::GetTaskRequest& request,
                                                              core::Completion<lf::a2a::v1::Task> completion,
                                                              const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::Task>(
      *async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::Task>& token) { StartGetTask(request, options, token); });
}
void ClientTransport::StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                                   const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> A2AClient::GetTaskAsync(const lf::a2a::v1::GetTaskRequest& request,
                                                        core::Completion<lf::a2a::v1::Task> completion,
                                                        const CallOptions& options) {
  auto transport = TransportSnapshot();
  Interceptors interceptors;
  {
    std::lock_guard lock(interceptor_mutex_);
    interceptors = interceptors_;
  }
  return StartClientCall<lf::a2a::v1::Task>(transport, core::protocol_methods::kGetTask, options, interceptors,
                                            std::move(completion), [&](core::Completion<lf::a2a::v1::Task> callback) {
                                              return transport->GetTaskAsync(request, std::move(callback), options);
                                            });
}

core::Result<core::AsyncHandle> ClientTransport::ListTasksAsync(const ListTasksRequest& request,
                                                                core::Completion<ListTasksResponse> completion,
                                                                const CallOptions& options) {
  return StartTransportCall<ListTasksResponse>(
      *async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<ListTasksResponse>& token) { StartListTasks(request, options, token); });
}
void ClientTransport::StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                                     const core::AsyncCompletion<ListTasksResponse>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> A2AClient::ListTasksAsync(const ListTasksRequest& request,
                                                          core::Completion<ListTasksResponse> completion,
                                                          const CallOptions& options) {
  auto transport = TransportSnapshot();
  Interceptors interceptors;
  {
    std::lock_guard lock(interceptor_mutex_);
    interceptors = interceptors_;
  }
  return StartClientCall<ListTasksResponse>(transport, core::protocol_methods::kListTasks, options, interceptors,
                                            std::move(completion), [&](core::Completion<ListTasksResponse> callback) {
                                              return transport->ListTasksAsync(request, std::move(callback), options);
                                            });
}

core::Result<core::AsyncHandle> ClientTransport::CancelTaskAsync(const lf::a2a::v1::CancelTaskRequest& request,
                                                                 core::Completion<lf::a2a::v1::Task> completion,
                                                                 const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::Task>(
      *async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::Task>& token) { StartCancelTask(request, options, token); });
}
void ClientTransport::StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                                      const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> A2AClient::CancelTaskAsync(const lf::a2a::v1::CancelTaskRequest& request,
                                                           core::Completion<lf::a2a::v1::Task> completion,
                                                           const CallOptions& options) {
  auto transport = TransportSnapshot();
  Interceptors interceptors;
  {
    std::lock_guard lock(interceptor_mutex_);
    interceptors = interceptors_;
  }
  return StartClientCall<lf::a2a::v1::Task>(transport, core::protocol_methods::kCancelTask, options, interceptors,
                                            std::move(completion), [&](core::Completion<lf::a2a::v1::Task> callback) {
                                              return transport->CancelTaskAsync(request, std::move(callback), options);
                                            });
}

core::Result<core::AsyncHandle> ClientTransport::CreateTaskPushNotificationConfigAsync(
    const lf::a2a::v1::TaskPushNotificationConfig& request,
    core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::TaskPushNotificationConfig>(
      *async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& token) {
        StartCreateTaskPushNotificationConfig(request, options, token);
      });
}
void ClientTransport::StartCreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> A2AClient::CreateTaskPushNotificationConfigAsync(
    const lf::a2a::v1::TaskPushNotificationConfig& request,
    core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options) {
  auto transport = TransportSnapshot();
  Interceptors interceptors;
  {
    std::lock_guard lock(interceptor_mutex_);
    interceptors = interceptors_;
  }
  return StartClientCall<lf::a2a::v1::TaskPushNotificationConfig>(
      transport, core::protocol_methods::kCreateTaskPushNotificationConfig, options, interceptors,
      std::move(completion), [&](core::Completion<lf::a2a::v1::TaskPushNotificationConfig> callback) {
        return transport->CreateTaskPushNotificationConfigAsync(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> ClientTransport::GetTaskPushNotificationConfigAsync(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
    core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::TaskPushNotificationConfig>(
      *async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& token) {
        StartGetTaskPushNotificationConfig(request, options, token);
      });
}
void ClientTransport::StartGetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> A2AClient::GetTaskPushNotificationConfigAsync(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
    core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options) {
  auto transport = TransportSnapshot();
  Interceptors interceptors;
  {
    std::lock_guard lock(interceptor_mutex_);
    interceptors = interceptors_;
  }
  return StartClientCall<lf::a2a::v1::TaskPushNotificationConfig>(
      transport, core::protocol_methods::kGetTaskPushNotificationConfig, options, interceptors, std::move(completion),
      [&](core::Completion<lf::a2a::v1::TaskPushNotificationConfig> callback) {
        return transport->GetTaskPushNotificationConfigAsync(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> ClientTransport::ListTaskPushNotificationConfigsAsync(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
    core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion, const CallOptions& options) {
  return StartTransportCall<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
      *async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
      [&](const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& token) {
        StartListTaskPushNotificationConfigs(request, options, token);
      });
}
void ClientTransport::StartListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> A2AClient::ListTaskPushNotificationConfigsAsync(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
    core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion, const CallOptions& options) {
  auto transport = TransportSnapshot();
  Interceptors interceptors;
  {
    std::lock_guard lock(interceptor_mutex_);
    interceptors = interceptors_;
  }
  return StartClientCall<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
      transport, core::protocol_methods::kListTaskPushNotificationConfigs, options, interceptors, std::move(completion),
      [&](core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> callback) {
        return transport->ListTaskPushNotificationConfigsAsync(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> ClientTransport::DeleteTaskPushNotificationConfigAsync(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, core::Completion<void> completion,
    const CallOptions& options) {
  return StartTransportCall<void>(*async_scope_, std::move(completion), options.timeout.value_or(DefaultAsyncTimeout()),
                                  [&](const core::AsyncCompletion<void>& token) {
                                    StartDeleteTaskPushNotificationConfig(request, options, token);
                                  });
}
void ClientTransport::StartDeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<void>& completion) {
  (void)request;
  (void)options;
  completion.Complete(core::Error::Validation(kUnsupportedAsync));
}
core::Result<core::AsyncHandle> A2AClient::DeleteTaskPushNotificationConfigAsync(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, core::Completion<void> completion,
    const CallOptions& options) {
  auto transport = TransportSnapshot();
  Interceptors interceptors;
  {
    std::lock_guard lock(interceptor_mutex_);
    interceptors = interceptors_;
  }
  return StartClientCall<void>(transport, core::protocol_methods::kDeleteTaskPushNotificationConfig, options,
                               interceptors, std::move(completion), [&](core::Completion<void> callback) {
                                 return transport->DeleteTaskPushNotificationConfigAsync(request, std::move(callback),
                                                                                         options);
                               });
}

}  // namespace a2a::client
