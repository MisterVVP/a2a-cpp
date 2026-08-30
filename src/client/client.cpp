// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/client.h"

#include <exception>
#include <ranges>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_methods.h"

namespace a2a::client {
StreamHandle::State::CallbackExecutionScope::CallbackExecutionScope(State& state) : state_(state) {
  std::lock_guard lock(state_.completion_mutex);
  state_.callback_thread_id = std::this_thread::get_id();
}

StreamHandle::State::CallbackExecutionScope::~CallbackExecutionScope() {
  std::lock_guard lock(state_.completion_mutex);
  state_.callback_thread_id = {};
}

void StreamHandle::State::RegisterCancelCallback(const std::function<void()>& callback) {
  bool cancellation_already_requested = false;
  {
    std::lock_guard lock(cancellation_mutex);
    cancellation_already_requested = cancel_requested.load();
    if (!cancellation_already_requested) {
      cancel_callback = callback;
    }
  }
  if (cancellation_already_requested) {
    callback();
  }
}

StreamHandle::StreamHandle(std::shared_ptr<State> state, WorkerThread worker)
    : state_(std::move(state)), worker_(std::move(worker)) {}

StreamHandle::StreamHandle(std::shared_ptr<State> state)
    : state_(std::move(state)), execution_mode_(ExecutionMode::kExecutor) {}

StreamHandle::StreamHandle(StreamHandle&&) noexcept = default;

StreamHandle& StreamHandle::operator=(StreamHandle&& other) noexcept {
  if (this != &other) {
    Cancel();
    state_ = std::move(other.state_);
    worker_ = std::move(other.worker_);
    execution_mode_ = other.execution_mode_;
  }
  return *this;
}

StreamHandle::~StreamHandle() { Cancel(); }

void StreamHandle::Cancel() {
  if (state_ == nullptr) {
    return;
  }

  state_->active.store(false);
  std::function<void()> cancel_callback;
  WorkerThread worker;
  {
    std::lock_guard lock(state_->cancellation_mutex);
    const bool first_cancellation = !state_->cancel_requested.exchange(true);
    if (first_cancellation) {
      cancel_callback = state_->cancel_callback;
    }
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
#if A2A_HAS_JTHREAD
      worker_.request_stop();
#endif
      worker = std::move(worker_);
    }
  }
  if (cancel_callback) {
    cancel_callback();
  }
  if (worker.joinable()) {
    worker.join();
  }
  std::unique_lock completion_lock(state_->completion_mutex);
  if (execution_mode_ == ExecutionMode::kExecutor && state_->callback_thread_id != std::this_thread::get_id()) {
    state_->completion_condition.wait(completion_lock, [this] { return state_->completed; });
  }
}

bool StreamHandle::IsActive() const {
  return state_ != nullptr && state_->active.load() && !state_->cancel_requested.load();
}

A2AClient::A2AClient(std::unique_ptr<ClientTransport> transport) : transport_(std::move(transport)) {}

core::Result<lf::a2a::v1::SendMessageResponse> A2AClient::SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                      const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kSendMessage, .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->SendMessage(request, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::Task> A2AClient::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                   const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kGetTask, .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->GetTask(request, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<ListTasksResponse> A2AClient::ListTasks(const ListTasksRequest& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kListTasks, .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->ListTasks(request, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::Task> A2AClient::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                      const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kCancelTask, .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->CancelTask(request, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> A2AClient::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kCreateTaskPushNotificationConfig,
                                  .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->CreateTaskPushNotificationConfig(request, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> A2AClient::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kGetTaskPushNotificationConfig,
                                  .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->GetTaskPushNotificationConfig(request, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> A2AClient::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kListTaskPushNotificationConfigs,
                                  .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->ListTaskPushNotificationConfigs(request, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<void> A2AClient::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kDeleteTaskPushNotificationConfig,
                                  .options = &options};
  RunBeforeInterceptors(context);
  const auto result = transport_->DeleteTaskPushNotificationConfig(request, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<std::unique_ptr<StreamHandle>> A2AClient::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "SendStreamingMessage", .options = &options};
  RunBeforeInterceptors(context);
  auto result = transport_->SendStreamingMessage(request, observer, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<std::unique_ptr<StreamHandle>> A2AClient::SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                     StreamObserver& observer,
                                                                     const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "SubscribeTask", .options = &options};
  RunBeforeInterceptors(context);
  auto result = transport_->SubscribeTask(request, observer, options);
  RunAfterInterceptors(context,
                       result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
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

void A2AClient::RunAfterInterceptors(const ClientCallContext& context, const ClientCallResult& result) const {
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
