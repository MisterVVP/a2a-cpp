// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/client.h"

#include <cstddef>
#include <exception>
#include <ranges>
#include <string_view>
#include <utility>

#include "a2a/client/async_client.h"
#include "a2a/client/async_transport.h"
#include "a2a/core/error.h"
#include "a2a/core/protocol_methods.h"
#include "client_state.h"

namespace a2a::client {
namespace {

class StreamCallbackThreadContext final {
 public:
  static void Enter() noexcept { ++Depth(); }
  static void Leave() noexcept { --Depth(); }
  [[nodiscard]] static bool IsActive() noexcept { return Depth() != 0U; }

 private:
  [[nodiscard]] static std::size_t& Depth() noexcept {
    static thread_local std::size_t depth = 0U;
    return depth;
  }
};

}  // namespace

StreamHandle::State::CallbackExecutionScope::CallbackExecutionScope(State& state) : state_(state) {
  std::lock_guard lock(state_.completion_mutex);
  state_.callback_thread_id = std::this_thread::get_id();
  StreamCallbackThreadContext::Enter();
  core::async_detail::EnterCallback();
}

StreamHandle::State::CallbackExecutionScope::~CallbackExecutionScope() {
  {
    std::lock_guard lock(state_.completion_mutex);
    state_.callback_thread_id = {};
  }
  StreamCallbackThreadContext::Leave();
  core::async_detail::LeaveCallback();
  state_.completion_condition.notify_all();
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

void StreamHandle::State::WaitForCallbackIdle() {
  std::unique_lock completion_lock(completion_mutex);
  if (StreamCallbackThreadContext::IsActive() || callback_thread_id == std::this_thread::get_id()) {
    return;
  }
  completion_condition.wait(completion_lock, [this] { return callback_thread_id == std::thread::id{}; });
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
  if (execution_mode_ == ExecutionMode::kExecutor) {
    // Cancellation makes queued callbacks no-ops. Only a callback that was
    // already executing can still reference the observer, so do not wait for
    // terminal dispatch on the process-wide executor.
    state_->WaitForCallbackIdle();
  }
}

bool StreamHandle::IsActive() const {
  const bool transport_is_shutdown =
      state_ != nullptr && state_->transport_shutdown != nullptr && state_->transport_shutdown->load();
  return state_ != nullptr && state_->active.load() && !state_->cancel_requested.load() && !transport_is_shutdown;
}

A2AClient::A2AClient(std::unique_ptr<ClientTransport> transport)
    : state_(std::make_shared<detail::ClientState>(std::move(transport))) {}

core::Result<lf::a2a::v1::SendMessageResponse> A2AClient::SendMessage(const lf::a2a::v1::SendMessageRequest& request,
                                                                      const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kSendMessage, .options = &options};
  detail::NotifyBefore(interceptors, context);
  const auto result = transport->SendMessage(request, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::Task> A2AClient::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                   const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kGetTask, .options = &options};
  detail::NotifyBefore(interceptors, context);
  const auto result = transport->GetTask(request, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<ListTasksResponse> A2AClient::ListTasks(const ListTasksRequest& request, const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kListTasks, .options = &options};
  detail::NotifyBefore(interceptors, context);
  const auto result = transport->ListTasks(request, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::Task> A2AClient::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                      const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kCancelTask, .options = &options};
  detail::NotifyBefore(interceptors, context);
  const auto result = transport->CancelTask(request, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> A2AClient::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kCreateTaskPushNotificationConfig,
                                  .options = &options};
  detail::NotifyBefore(interceptors, context);
  const auto result = transport->CreateTaskPushNotificationConfig(request, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> A2AClient::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kGetTaskPushNotificationConfig,
                                  .options = &options};
  detail::NotifyBefore(interceptors, context);
  const auto result = transport->GetTaskPushNotificationConfig(request, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> A2AClient::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kListTaskPushNotificationConfigs,
                                  .options = &options};
  detail::NotifyBefore(interceptors, context);
  const auto result = transport->ListTaskPushNotificationConfigs(request, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<void> A2AClient::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = core::protocol_methods::kDeleteTaskPushNotificationConfig,
                                  .options = &options};
  detail::NotifyBefore(interceptors, context);
  const auto result = transport->DeleteTaskPushNotificationConfig(request, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<std::unique_ptr<StreamHandle>> A2AClient::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "SendStreamingMessage", .options = &options};
  detail::NotifyBefore(interceptors, context);
  auto result = transport->SendStreamingMessage(request, observer, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

core::Result<std::unique_ptr<StreamHandle>> A2AClient::SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                     StreamObserver& observer,
                                                                     const CallOptions& options) {
  const auto state = state_;
  auto transport = state->TransportSnapshot();
  const auto interceptors = state->InterceptorSnapshot();
  if (transport == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  const ClientCallContext context{.operation = "SubscribeTask", .options = &options};
  detail::NotifyBefore(interceptors, context);
  auto result = transport->SubscribeTask(request, observer, options);
  detail::NotifyAfter(interceptors, context,
                      result.ok() ? ClientCallResult{} : ClientCallResult{.ok = false, .error = result.error()});
  return result;
}

void A2AClient::AddInterceptor(std::shared_ptr<ClientInterceptor> interceptor) {
  state_->AddInterceptor(std::move(interceptor));
}
AsyncClient A2AClient::Async() const { return AsyncClient(state_); }
core::Result<void> A2AClient::Destroy() { return state_->Shutdown(); }
A2AClient::~A2AClient() {
  try {
    (void)Destroy();
  } catch (...) {
    state_->ReleaseClosingTransport();
  }
}
namespace detail {
void NotifyBefore(const Interceptors& interceptors, const ClientCallContext& context) {
  for (const auto& interceptor : interceptors) {
    try {
      interceptor->BeforeCall(context);
    } catch (...) {
      continue;
    }
  }
}
void NotifyAfter(const Interceptors& interceptors, const ClientCallContext& context, const ClientCallResult& result) {
  for (const auto& interceptor : std::ranges::reverse_view(interceptors)) {
    try {
      interceptor->AfterCall(context, result);
    } catch (...) {
      continue;
    }
  }
}

std::shared_ptr<ClientTransport> ClientState::TransportSnapshot() const {
  std::lock_guard lock(transport_mutex);
  return transport;
}
std::vector<std::shared_ptr<ClientInterceptor>> ClientState::InterceptorSnapshot() const {
  std::lock_guard lock(interceptor_mutex);
  return interceptors;
}
void ClientState::AddInterceptor(std::shared_ptr<ClientInterceptor> interceptor) {
  if (!interceptor) {
    return;
  }
  std::lock_guard lock(interceptor_mutex);
  interceptors.push_back(std::move(interceptor));
}
void ClientState::ReleaseClosingTransport() {
  std::shared_ptr<ClientTransport> released;
  {
    std::lock_guard lock(transport_mutex);
    released = std::move(closing_transport);
  }
}
core::Result<void> ClientState::Shutdown() {
  std::shared_ptr<ClientTransport> snapshot;
  {
    std::lock_guard lock(transport_mutex);
    if (transport) {
      closing_transport = std::move(transport);
    }
    snapshot = closing_transport;
  }
  if (!snapshot) {
    return {};
  }
  const auto async = snapshot->Async();
  core::Result<void> result;
  try {
    result = snapshot->Shutdown();
  } catch (...) {
    if (async) {
      async->Shutdown();
    }
    throw;
  }
  if (async) {
    async->Shutdown();
  }
  // Reentrant shutdown stays observable to a later external draining call.
  if (!core::async_detail::IsCallbackThread()) {
    ReleaseClosingTransport();
  }
  return result;
}
}  // namespace detail
}  // namespace a2a::client
