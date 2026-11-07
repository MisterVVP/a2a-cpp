// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#if defined(__cpp_lib_jthread) && (__cpp_lib_jthread >= 201911L)
#define A2A_HAS_JTHREAD 1
#else
#define A2A_HAS_JTHREAD 0
#endif
#include <vector>

#include "a2a/client/call_options.h"
#include "a2a/core/async_operation.h"
#include "a2a/core/non_copyable.h"
#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

#if defined(_WIN32) && defined(SendMessage)
#undef SendMessage
#endif

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

class StreamHandle final : private core::NonCopyable {
 public:
  struct State final {
    class CallbackExecutionScope final : private core::NonCopyable {
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
  static constexpr std::chrono::milliseconds kDefaultAsyncTimeout{30000};
  virtual ~ClientTransport() = default;

  // Accepted calls complete exactly once on shared completion workers.
  [[nodiscard]] core::Result<core::AsyncHandle> SendMessageAsync(
      const lf::a2a::v1::SendMessageRequest& request, core::Completion<lf::a2a::v1::SendMessageResponse> completion,
      const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> GetTaskAsync(const lf::a2a::v1::GetTaskRequest& request,
                                                             core::Completion<lf::a2a::v1::Task> completion,
                                                             const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> ListTasksAsync(const ListTasksRequest& request,
                                                               core::Completion<ListTasksResponse> completion,
                                                               const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> CancelTaskAsync(const lf::a2a::v1::CancelTaskRequest& request,
                                                                core::Completion<lf::a2a::v1::Task> completion,
                                                                const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> CreateTaskPushNotificationConfigAsync(
      const lf::a2a::v1::TaskPushNotificationConfig& request,
      core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> GetTaskPushNotificationConfigAsync(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> ListTaskPushNotificationConfigsAsync(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion,
      const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> DeleteTaskPushNotificationConfigAsync(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, core::Completion<void> completion,
      const CallOptions& options = {});

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

  [[nodiscard]] virtual core::Result<void> Shutdown() {
    async_scope_->Shutdown();
    return {};
  }

 protected:
  std::shared_ptr<core::AsyncScope> async_scope_ = std::make_shared<core::AsyncScope>();
  [[nodiscard]] virtual std::chrono::milliseconds DefaultAsyncTimeout() const { return kDefaultAsyncTimeout; }
  virtual void StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
                                const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion);
  virtual void StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                            const core::AsyncCompletion<lf::a2a::v1::Task>& completion);
  virtual void StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                              const core::AsyncCompletion<ListTasksResponse>& completion);
  virtual void StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                               const core::AsyncCompletion<lf::a2a::v1::Task>& completion);
  virtual void StartCreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion);
  virtual void StartGetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion);
  virtual void StartListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion);
  virtual void StartDeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options,
      const core::AsyncCompletion<void>& completion);
};

class A2AClient final {
 public:
  explicit A2AClient(std::unique_ptr<ClientTransport> transport);
  ~A2AClient();

  // Accepted calls complete exactly once on shared completion workers.
  [[nodiscard]] core::Result<core::AsyncHandle> SendMessageAsync(
      const lf::a2a::v1::SendMessageRequest& request, core::Completion<lf::a2a::v1::SendMessageResponse> completion,
      const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> GetTaskAsync(const lf::a2a::v1::GetTaskRequest& request,
                                                             core::Completion<lf::a2a::v1::Task> completion,
                                                             const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> ListTasksAsync(const ListTasksRequest& request,
                                                               core::Completion<ListTasksResponse> completion,
                                                               const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> CancelTaskAsync(const lf::a2a::v1::CancelTaskRequest& request,
                                                                core::Completion<lf::a2a::v1::Task> completion,
                                                                const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> CreateTaskPushNotificationConfigAsync(
      const lf::a2a::v1::TaskPushNotificationConfig& request,
      core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> GetTaskPushNotificationConfigAsync(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      core::Completion<lf::a2a::v1::TaskPushNotificationConfig> completion, const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> ListTaskPushNotificationConfigsAsync(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      core::Completion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> completion,
      const CallOptions& options = {});
  [[nodiscard]] core::Result<core::AsyncHandle> DeleteTaskPushNotificationConfigAsync(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, core::Completion<void> completion,
      const CallOptions& options = {});

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
  [[nodiscard]] std::shared_ptr<ClientTransport> TransportSnapshot() const;
  mutable std::mutex transport_mutex_;
  void RunBeforeInterceptors(const ClientCallContext& context) const;
  void RunAfterInterceptors(const ClientCallContext& context, const ClientCallResult& result) const;

  std::shared_ptr<ClientTransport> transport_;
  std::shared_ptr<ClientTransport> closing_transport_;
  mutable std::mutex interceptor_mutex_;
  std::vector<std::shared_ptr<ClientInterceptor>> interceptors_;
};

}  // namespace a2a::client
