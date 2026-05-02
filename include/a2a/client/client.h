#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "a2a/client/call_options.h"
#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::client {

class HttpJsonTransport;
class GrpcTransport;

struct ListTasksRequest final {
  std::size_t page_size = 0;
  std::string page_token;
};

struct ListTasksResponse final {
  std::vector<lf::a2a::v1::Task> tasks;
  std::string next_page_token;
};

struct ClientCallContext final {
  std::string_view operation;
  const CallOptions* options = nullptr;
};

struct ClientCallResult final {
  bool ok = true;
  std::optional<core::Error> error = std::nullopt;
};

class ClientInterceptor {
 public:
  virtual ~ClientInterceptor() = default;
  virtual void BeforeCall(const ClientCallContext& context) { (void)context; }
  virtual void AfterCall(const ClientCallContext& context, const ClientCallResult& result) {
    (void)context;
    (void)result;
  }
};

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
  friend class GrpcTransport;

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
  [[nodiscard]] virtual core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                                  const CallOptions& options) = 0;
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

  [[nodiscard]] virtual core::Result<void> Shutdown() { return {}; }
};

class A2AClient final {
 public:
  explicit A2AClient(std::unique_ptr<ClientTransport> transport);

  [[nodiscard]] core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                        const CallOptions& options = {});
  [[nodiscard]] core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
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

  void AddInterceptor(std::shared_ptr<ClientInterceptor> interceptor);
  [[nodiscard]] core::Result<void> Destroy();

 private:
  void RunBeforeInterceptors(const ClientCallContext& context) const;
  void RunAfterInterceptors(const ClientCallContext& context, const ClientCallResult& result) const;

  std::unique_ptr<ClientTransport> transport_;
  mutable std::mutex interceptor_mutex_;
  std::vector<std::shared_ptr<ClientInterceptor>> interceptors_;
};

}  // namespace a2a::client
