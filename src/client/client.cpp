#include "a2a/client/client.h"

#include <exception>
#include <ranges>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"

namespace a2a::client {
StreamHandle::StreamHandle(std::shared_ptr<State> state, std::jthread worker)
    : state_(std::move(state)), worker_(std::move(worker)) {}

StreamHandle::StreamHandle(StreamHandle&&) noexcept = default;

StreamHandle& StreamHandle::operator=(StreamHandle&&) noexcept = default;

StreamHandle::~StreamHandle() { Cancel(); }

void StreamHandle::Cancel() {
  if (state_ != nullptr) {
    state_->cancel_requested.store(true);
    state_->active.store(false);
  }
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

bool StreamHandle::IsActive() const {
  return state_ != nullptr && state_->active.load() && !state_->cancel_requested.load();
}

A2AClient::A2AClient(std::unique_ptr<ClientTransport> transport)
    : transport_(std::move(transport)) {}

core::Result<lf::a2a::v1::SendMessageResponse> A2AClient::SendMessage(
    const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "SendMessage", .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->SendMessage(request, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::Task> A2AClient::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                   const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "GetTask", .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->GetTask(request, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<ListTasksResponse> A2AClient::ListTasks(const ListTasksRequest& request,
                                                     const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "ListTasks", .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->ListTasks(request, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::Task> A2AClient::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                      const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "CancelTask", .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->CancelTask(request, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> A2AClient::SetTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "SetTaskPushNotificationConfig",
                                  .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->SetTaskPushNotificationConfig(request, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> A2AClient::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "GetTaskPushNotificationConfig",
                                  .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->GetTaskPushNotificationConfig(request, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
A2AClient::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
    const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "ListTaskPushNotificationConfigs",
                                  .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->ListTaskPushNotificationConfigs(request, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<void> A2AClient::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
    const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "DeleteTaskPushNotificationConfig",
                                  .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->DeleteTaskPushNotificationConfig(request, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<std::unique_ptr<StreamHandle>> A2AClient::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
    const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "SendStreamingMessage", .options = &options};
  RunBeforeInterceptors(context);
  auto result = transport_->SendStreamingMessage(request, observer, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<std::unique_ptr<StreamHandle>> A2AClient::SubscribeTask(
    const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer,
    const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "SubscribeTask", .options = &options};
  RunBeforeInterceptors(context);
  auto result = transport_->SubscribeTask(request, observer, options);
  RunAfterInterceptors(context, result.ok()
                                    ? ClientCallResult{}
                                    : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

void A2AClient::AddInterceptor(std::shared_ptr<ClientInterceptor> interceptor) {
  if (interceptor == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(interceptor_mutex_);
  interceptors_.push_back(std::move(interceptor));
}

core::Result<void> A2AClient::Destroy() {
  if (transport_ == nullptr) {
    return {};
  }
  const auto shutdown = transport_->Shutdown();
  transport_.reset();
  return shutdown;
}

void A2AClient::RunBeforeInterceptors(const ClientCallContext& context) const {
  std::lock_guard<std::mutex> lock(interceptor_mutex_);
  for (const auto& interceptor : interceptors_) {
    if (interceptor == nullptr) {
      continue;
    }
    try {
      interceptor->BeforeCall(context);
    } catch (const std::exception&) {
      continue;
    } catch (...) {
      continue;
    }
  }
}

void A2AClient::RunAfterInterceptors(const ClientCallContext& context,
                                     const ClientCallResult& result) const {
  std::lock_guard<std::mutex> lock(interceptor_mutex_);
  for (const auto& interceptor : std::ranges::reverse_view(interceptors_)) {
    if (interceptor == nullptr) {
      continue;
    }
    try {
      interceptor->AfterCall(context, result);
    } catch (const std::exception&) {
      continue;
    } catch (...) {
      continue;
    }
  }
}

}  // namespace a2a::client
