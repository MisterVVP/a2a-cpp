#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "a2a/client/call_options.h"
#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::client {

class HttpJsonTransport;

class StreamObserver {
 public:
  virtual ~StreamObserver() = default;

  virtual void OnEvent(const lf::a2a::v1::StreamResponse& response) = 0;
  virtual void OnError(const core::Error& error) = 0;
  virtual void OnCompleted() = 0;
};

class StreamHandle final {
 public:
  struct State final {
    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> active{true};
  };

  StreamHandle() = delete;
  StreamHandle(const StreamHandle&) = delete;
  StreamHandle& operator=(const StreamHandle&) = delete;
  StreamHandle(StreamHandle&&) noexcept;
  StreamHandle& operator=(StreamHandle&&) noexcept;
  ~StreamHandle();

  void Cancel();
  [[nodiscard]] bool IsActive() const;

 private:
  friend class HttpJsonTransport;

  explicit StreamHandle(std::shared_ptr<State> state, std::jthread worker);

  std::shared_ptr<State> state_;
  std::jthread worker_;
};

class ClientTransport {
 public:
  virtual ~ClientTransport() = default;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> GetTask(
      const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> CancelTask(
      const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig>
  SetTaskPushNotificationConfig(const lf::a2a::v1::TaskPushNotificationConfig& request,
                                const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig>
  GetTaskPushNotificationConfig(const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
                                const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const CallOptions& options) = 0;

  // Threading contract: observer callbacks run on transport-managed background
  // threads. The caller must keep observer alive until stream completion,
  // cancellation, or handle destruction.
  [[nodiscard]] virtual core::Result<std::unique_ptr<StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
      const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<std::unique_ptr<StreamHandle>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer,
      const CallOptions& options) = 0;
};

class A2AClient final {
 public:
  explicit A2AClient(std::unique_ptr<ClientTransport> transport);

  [[nodiscard]] core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                        const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::Task> CancelTask(
      const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> SetTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      const CallOptions& options = {});

  [[nodiscard]] core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const CallOptions& options = {});

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
      const CallOptions& options = {});

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer,
      const CallOptions& options = {});

 private:
  std::unique_ptr<ClientTransport> transport_;
};

}  // namespace a2a::client
