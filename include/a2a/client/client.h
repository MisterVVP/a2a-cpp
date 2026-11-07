// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#if defined(__cpp_lib_jthread) && (__cpp_lib_jthread >= 201911L)
#define A2A_HAS_JTHREAD 1
#else
#define A2A_HAS_JTHREAD 0
#endif

#include "a2a/client/call_options.h"
#include "a2a/client/client_types.h"
#include "a2a/core/non_copyable.h"
#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

#if defined(_WIN32) && defined(SendMessage)
#undef SendMessage
#endif

namespace a2a::client {

namespace detail {
struct ClientState;
}
class AsyncClient;
class AsyncClientTransport;
class HttpJsonTransport;
class GrpcTransport;

class StreamObserver {
 public:
  virtual ~StreamObserver() = default;

  virtual void OnEvent(const lf::a2a::v1::StreamResponse& response) = 0;
  virtual void OnError(const core::Error& error) = 0;
  virtual void OnCompleted() = 0;
};

class StreamHandle final : private core::NonCopyable {
 public:
  struct State final {
    class CallbackExecutionScope final : private core::NonCopyableOrMovable {
     public:
      explicit CallbackExecutionScope(State& state);
      ~CallbackExecutionScope();

     private:
      State& state_;
    };

    std::atomic<bool> cancel_requested{false};
    std::atomic<bool> active{true};
    std::shared_ptr<std::atomic<bool>> transport_shutdown;
    std::mutex cancellation_mutex;
    std::function<void()> cancel_callback;
    std::mutex completion_mutex;
    std::condition_variable completion_condition;
    std::thread::id callback_thread_id;

    void RegisterCancelCallback(const std::function<void()>& callback);
    void WaitForCallbackIdle();
  };

  StreamHandle() = delete;
  StreamHandle(StreamHandle&&) noexcept;
  StreamHandle& operator=(StreamHandle&&) noexcept;
  ~StreamHandle();

  // Cancellation from inside a stream callback does not wait for callbacks already running on other streams.
  // Keep observer-owned state alive until those concurrent callbacks return.
  void Cancel();
  [[nodiscard]] bool IsActive() const;

 private:
  friend class HttpJsonTransport;
  friend class JsonRpcTransport;
  friend class GrpcTransport;

#if A2A_HAS_JTHREAD
  using WorkerThread = std::jthread;
#else
  using WorkerThread = std::thread;
#endif

  enum class ExecutionMode : std::uint8_t {
    kOwnedWorker,
    kExecutor,
  };

  explicit StreamHandle(std::shared_ptr<State> state, WorkerThread worker);
  explicit StreamHandle(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
  WorkerThread worker_;
  ExecutionMode execution_mode_ = ExecutionMode::kOwnedWorker;
};

class ClientTransport {
 public:
  virtual ~ClientTransport() = default;
  // Returns the composed async implementation, or null for sync-only transports.
  [[nodiscard]] virtual std::shared_ptr<AsyncClientTransport> Async() const { return {}; }

  [[nodiscard]] virtual core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                const CallOptions& options) = 0;
  [[nodiscard]] virtual core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                                  const CallOptions& options) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                                   const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
                                  const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) = 0;

  // Threading contract: observer callbacks run on transport-managed background
  // threads. The caller must keep observer alive until stream completion,
  // cancellation, or handle destruction.
  [[nodiscard]] virtual core::Result<std::unique_ptr<StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<std::unique_ptr<StreamHandle>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer, const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<void> Shutdown() { return {}; }
};

class A2AClient final : private core::NonCopyableOrMovable {
 public:
  explicit A2AClient(std::unique_ptr<ClientTransport> transport);
  ~A2AClient();

  // Include async_client.h to use the returned callback facade.
  [[nodiscard]] AsyncClient Async() const;

  [[nodiscard]] core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                        const CallOptions& options = {});
  [[nodiscard]] core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                          const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                           const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options = {});

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                          StreamObserver& observer,
                                                                          const CallOptions& options = {});

  void AddInterceptor(std::shared_ptr<ClientInterceptor> interceptor);
  [[nodiscard]] core::Result<void> Destroy();

 private:
  std::shared_ptr<detail::ClientState> state_;
};

}  // namespace a2a::client
