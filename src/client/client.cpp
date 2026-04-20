#include "a2a/client/client.h"

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
  return transport_->SendMessage(request, options);
}

core::Result<lf::a2a::v1::Task> A2AClient::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                   const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  return transport_->GetTask(request, options);
}

core::Result<lf::a2a::v1::Task> A2AClient::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                      const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  return transport_->CancelTask(request, options);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> A2AClient::SetTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  return transport_->SetTaskPushNotificationConfig(request, options);
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> A2AClient::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  return transport_->GetTaskPushNotificationConfig(request, options);
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
A2AClient::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
    const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  return transport_->ListTaskPushNotificationConfigs(request, options);
}

core::Result<void> A2AClient::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
    const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  return transport_->DeleteTaskPushNotificationConfig(request, options);
}

core::Result<std::unique_ptr<StreamHandle>> A2AClient::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
    const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  return transport_->SendStreamingMessage(request, observer, options);
}

core::Result<std::unique_ptr<StreamHandle>> A2AClient::SubscribeTask(
    const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer,
    const CallOptions& options) {
  if (transport_ == nullptr) {
    return core::Error::Internal("Client transport is not configured");
  }
  return transport_->SubscribeTask(request, observer, options);
}

}  // namespace a2a::client
