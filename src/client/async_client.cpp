// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/async_client.h"

#include <utility>

#include "a2a/client/async_transport.h"
#include "a2a/core/protocol_methods.h"
#include "client_state.h"

namespace a2a::client {
namespace {
constexpr const char* kUnsupportedAsync = "transport does not implement async unary calls";
constexpr const char* kMissingCompletion = "async completion callback is required";
constexpr const char* kMissingTransport = "Client transport is not configured";
constexpr const char* kStartFailed = "async transport initiation threw an exception";
using Interceptors = std::vector<std::shared_ptr<ClientInterceptor>>;

using detail::NotifyAfter;
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
core::Result<core::AsyncHandle> StartClientCall(const std::shared_ptr<detail::ClientState>& state,
                                                std::string_view operation, const CallOptions& options,
                                                core::Completion<T> callback, const Initiate& initiate) {
  const auto transport = state ? state->TransportSnapshot() : nullptr;
  if (!transport) {
    return core::Error::Internal(kMissingTransport);
  }
  if (!callback) {
    return core::Error::Validation(kMissingCompletion);
  }
  const auto interceptors = state->InterceptorSnapshot();
  const ClientCallContext context{.operation = operation, .options = &options};
  detail::NotifyBefore(interceptors, context);
  if (!interceptors.empty()) {
    callback = InterceptCompletion<T>(operation, options, interceptors, std::move(callback));
  }
  auto started = [&]() -> core::Result<core::AsyncHandle> {
    try {
      auto async = transport->Async();
      if (!async) {
        return core::Error::Validation(kUnsupportedAsync);
      }
      return initiate(*async, std::move(callback));
    } catch (...) {
      return core::Error::Internal(kStartFailed);
    }
  }();
  if (!started.ok()) {
    NotifyAfter(interceptors, context, ClientCallResult{.ok = false, .error = started.error()});
  }
  return started;
}
}  // namespace

core::Result<core::AsyncHandle> AsyncClient::SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                         core::Completion<lf::a2a::v1::SendMessageResponse> completion,
                                                         const CallOptions& options) {
  const auto state = state_;
  return StartClientCall<lf::a2a::v1::SendMessageResponse>(
      state, core::protocol_methods::kSendMessage, options, std::move(completion),
      [&](AsyncClientTransport& transport, core::Completion<lf::a2a::v1::SendMessageResponse> callback) {
        return transport.SendMessage(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> AsyncClient::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                     core::Completion<lf::a2a::v1::Task> completion,
                                                     const CallOptions& options) {
  const auto state = state_;
  return StartClientCall<lf::a2a::v1::Task>(
      state, core::protocol_methods::kGetTask, options, std::move(completion),
      [&](AsyncClientTransport& transport, core::Completion<lf::a2a::v1::Task> callback) {
        return transport.GetTask(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> AsyncClient::ListTasks(const ListTasksRequest& request,
                                                       core::Completion<ListTasksResponse> completion,
                                                       const CallOptions& options) {
  const auto state = state_;
  return StartClientCall<ListTasksResponse>(
      state, core::protocol_methods::kListTasks, options, std::move(completion),
      [&](AsyncClientTransport& transport, core::Completion<ListTasksResponse> callback) {
        return transport.ListTasks(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> AsyncClient::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                        core::Completion<lf::a2a::v1::Task> completion,
                                                        const CallOptions& options) {
  const auto state = state_;
  return StartClientCall<lf::a2a::v1::Task>(
      state, core::protocol_methods::kCancelTask, options, std::move(completion),
      [&](AsyncClientTransport& transport, core::Completion<lf::a2a::v1::Task> callback) {
        return transport.CancelTask(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> AsyncClient::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request,
    core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options) {
  const auto state = state_;
  return StartClientCall<lf::a2a::v1::TaskPushNotificationConfig>(
      state, core::protocol_methods::kCreateTaskPushNotificationConfig, options, std::move(completion),
      [&](AsyncClientTransport& transport, core::Completion<lf::a2a::v1::TaskPushNotificationConfig> callback) {
        return transport.CreateTaskPushNotificationConfig(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> AsyncClient::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
    core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options) {
  const auto state = state_;
  return StartClientCall<lf::a2a::v1::TaskPushNotificationConfig>(
      state, core::protocol_methods::kGetTaskPushNotificationConfig, options, std::move(completion),
      [&](AsyncClientTransport& transport, core::Completion<lf::a2a::v1::TaskPushNotificationConfig> callback) {
        return transport.GetTaskPushNotificationConfig(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> AsyncClient::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
    core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion, const CallOptions& options) {
  const auto state = state_;
  return StartClientCall<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
      state, core::protocol_methods::kListTaskPushNotificationConfigs, options, std::move(completion),
      [&](AsyncClientTransport& transport,
          core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> callback) {
        return transport.ListTaskPushNotificationConfigs(request, std::move(callback), options);
      });
}

core::Result<core::AsyncHandle> AsyncClient::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, core::Completion<void> completion,
    const CallOptions& options) {
  const auto state = state_;
  return StartClientCall<void>(
      state, core::protocol_methods::kDeleteTaskPushNotificationConfig, options, std::move(completion),
      [&](AsyncClientTransport& transport, core::Completion<void> callback) {
        return transport.DeleteTaskPushNotificationConfig(request, std::move(callback), options);
      });
}

}  // namespace a2a::client
