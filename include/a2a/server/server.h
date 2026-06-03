// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/timestamp.pb.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "a2a/core/protocol_errors.h"
#include "a2a/core/result.h"
#include "a2a/server/task_id_generator.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

struct RequestContext final {
  std::optional<std::string> request_id;
  std::optional<std::string> remote_address;
  std::unordered_map<std::string, std::string> auth_metadata;
  std::unordered_map<std::string, std::string> client_headers;
};

[[nodiscard]] std::unordered_map<std::string, std::string> ExtractAuthMetadata(
    const std::unordered_map<std::string, std::string>& headers);

struct ListTasksRequest final {
  std::size_t page_size = 0;
  std::string page_token;
  std::string context_id;
  std::optional<lf::a2a::v1::TaskState> status_filter;
  std::optional<google::protobuf::Timestamp> status_timestamp_after;
  std::optional<std::size_t> history_length;
  bool include_artifacts = false;

  ListTasksRequest() noexcept = default;

  ListTasksRequest(std::size_t page_size_value, std::string page_token_value)
      : page_size(page_size_value), page_token(std::move(page_token_value)) {}

  ListTasksRequest(const ListTasksRequest& other)
      : page_size(other.page_size),
        page_token(other.page_token),
        context_id(other.context_id),
        status_filter(other.status_filter),
        status_timestamp_after(other.status_timestamp_after),
        history_length(other.history_length),
        include_artifacts(other.include_artifacts) {}

  ListTasksRequest& operator=(const ListTasksRequest& other) {
    if (this != &other) {
      page_size = other.page_size;
      page_token = other.page_token;
      context_id = other.context_id;
      status_filter = other.status_filter;
      status_timestamp_after = other.status_timestamp_after;
      history_length = other.history_length;
      include_artifacts = other.include_artifacts;
    }
    return *this;
  }

  ListTasksRequest(ListTasksRequest&& other) noexcept
      : page_size(other.page_size),
        page_token(std::move(other.page_token)),
        context_id(std::move(other.context_id)),
        status_filter(other.status_filter),
        status_timestamp_after(std::move(other.status_timestamp_after)),
        history_length(other.history_length),
        include_artifacts(other.include_artifacts) {
    other.page_size = 0;
    other.include_artifacts = false;
  }

  ListTasksRequest& operator=(ListTasksRequest&& other) noexcept {
    if (this != &other) {
      page_size = other.page_size;
      page_token = std::move(other.page_token);
      context_id = std::move(other.context_id);
      status_filter = other.status_filter;
      status_timestamp_after = std::move(other.status_timestamp_after);
      history_length = other.history_length;
      include_artifacts = other.include_artifacts;
      other.page_size = 0;
      other.include_artifacts = false;
    }
    return *this;
  }
};

struct ListTasksResponse final {
  std::vector<lf::a2a::v1::Task> tasks;
  std::size_t page_size = 0;
  std::size_t total_size = 0;
  std::string next_page_token;
};

class ServerStreamSession {
 public:
  virtual ~ServerStreamSession() = default;

  [[nodiscard]] virtual core::Result<std::optional<lf::a2a::v1::StreamResponse>> Next() = 0;
};

class AgentExecutor {
 public:
  virtual ~AgentExecutor() = default;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<std::unique_ptr<ServerStreamSession>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                                  RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                                   RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, RequestContext& context) {
    (void)request;
    (void)context;
    return core::protocol_errors::PushNotificationNotSupported();
  }

  [[nodiscard]] virtual core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, RequestContext& context) {
    (void)request;
    (void)context;
    return core::protocol_errors::PushNotificationNotSupported();
  }

  [[nodiscard]] virtual core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
                                  RequestContext& context) {
    (void)request;
    (void)context;
    return core::protocol_errors::PushNotificationNotSupported();
  }

  [[nodiscard]] virtual core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, RequestContext& context) {
    (void)request;
    (void)context;
    return core::protocol_errors::PushNotificationNotSupported();
  }
};

enum class DispatcherOperation : std::uint8_t {
  kSendMessage,
  kSendStreamingMessage,
  kGetTask,
  kListTasks,
  kCancelTask,
  kCreateTaskPushNotificationConfig,
  kGetTaskPushNotificationConfig,
  kListTaskPushNotificationConfigs,
  kDeleteTaskPushNotificationConfig,
};

struct DispatchRequest final {
  DispatcherOperation operation = DispatcherOperation::kSendMessage;
  std::variant<lf::a2a::v1::SendMessageRequest, lf::a2a::v1::GetTaskRequest, ListTasksRequest,
               lf::a2a::v1::CancelTaskRequest, lf::a2a::v1::TaskPushNotificationConfig,
               lf::a2a::v1::GetTaskPushNotificationConfigRequest, lf::a2a::v1::ListTaskPushNotificationConfigsRequest,
               lf::a2a::v1::DeleteTaskPushNotificationConfigRequest>
      payload = ListTasksRequest{};
};

using DispatchPayload = std::variant<lf::a2a::v1::SendMessageResponse, std::unique_ptr<ServerStreamSession>,
                                     lf::a2a::v1::Task, ListTasksResponse, lf::a2a::v1::TaskPushNotificationConfig,
                                     lf::a2a::v1::ListTaskPushNotificationConfigsResponse, std::monostate>;

class DispatchResponse final {
 public:
  explicit DispatchResponse(const lf::a2a::v1::SendMessageResponse& payload) : payload_(payload) {}
  explicit DispatchResponse(lf::a2a::v1::SendMessageResponse&& payload) : payload_(std::move(payload)) {}
  explicit DispatchResponse(std::unique_ptr<ServerStreamSession> payload) : payload_(std::move(payload)) {}
  explicit DispatchResponse(const lf::a2a::v1::Task& payload) : payload_(payload) {}
  explicit DispatchResponse(lf::a2a::v1::Task&& payload) : payload_(std::move(payload)) {}
  explicit DispatchResponse(const ListTasksResponse& payload) : payload_(payload) {}
  explicit DispatchResponse(ListTasksResponse&& payload) : payload_(std::move(payload)) {}
  explicit DispatchResponse(const lf::a2a::v1::TaskPushNotificationConfig& payload) : payload_(payload) {}
  explicit DispatchResponse(lf::a2a::v1::TaskPushNotificationConfig&& payload) : payload_(std::move(payload)) {}
  explicit DispatchResponse(const lf::a2a::v1::ListTaskPushNotificationConfigsResponse& payload) : payload_(payload) {}
  explicit DispatchResponse(lf::a2a::v1::ListTaskPushNotificationConfigsResponse&& payload)
      : payload_(std::move(payload)) {}
  DispatchResponse() : payload_(std::monostate{}) {}

  [[nodiscard]] const DispatchPayload& payload() const noexcept { return payload_; }
  [[nodiscard]] DispatchPayload& payload() noexcept { return payload_; }

 private:
  DispatchPayload payload_;
};

class ServerInterceptor {
 public:
  virtual ~ServerInterceptor() = default;

  virtual core::Result<void> BeforeDispatch(const DispatchRequest& request, RequestContext& context) {
    (void)request;
    (void)context;
    return {};
  }

  virtual void AfterDispatch(const DispatchRequest& request, RequestContext& context,
                             const core::Result<DispatchResponse>& result) {
    (void)request;
    (void)context;
    (void)result;
  }
};

class Dispatcher final {
 public:
  explicit Dispatcher(AgentExecutor* executor);
  explicit Dispatcher(AgentExecutor* executor, std::vector<std::shared_ptr<ServerInterceptor>> interceptors);

  [[nodiscard]] core::Result<DispatchResponse> Dispatch(const DispatchRequest& request, RequestContext& context) const;
  void AddInterceptor(std::shared_ptr<ServerInterceptor> interceptor);

 private:
  void RunAfterInterceptors(const DispatchRequest& request, RequestContext& context,
                            const core::Result<DispatchResponse>& result) const;

  AgentExecutor* executor_ = nullptr;
  mutable std::shared_mutex interceptor_mutex_;
  std::vector<std::shared_ptr<ServerInterceptor>> interceptors_;
};

class TaskStore {
 public:
  enum class HistoryAppendPolicy {
    // Appends every request in arrival order (no dedupe).
    kNoDedup,
    // Drops a request only when message_id is present and both message_id + message fingerprint match.
    kDedupByMessageId,
    // Drops by message_id+fingerprint when message_id is present; otherwise drops by fingerprint alone.
    kDedupByIdOrFingerprint,
  };

  struct HistoryDedupeEvent final {
    enum class Reason : std::uint8_t {
      kDuplicateMessageIdAndFingerprint,
      kDuplicateFingerprintWithoutMessageId,
    };

    std::string task_id;
    std::string message_id;
    HistoryAppendPolicy policy = HistoryAppendPolicy::kNoDedup;
    Reason reason = Reason::kDuplicateMessageIdAndFingerprint;
  };

  struct HistoryTelemetrySnapshot final {
    std::size_t dedupe_dropped_total = 0;
    std::size_t dedupe_dropped_by_message_id_and_fingerprint = 0;
    std::size_t dedupe_dropped_by_fingerprint_without_message_id = 0;
  };

  virtual ~TaskStore() = default;

  [[nodiscard]] virtual core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> Get(std::string_view id) const = 0;
  [[nodiscard]] virtual core::Result<ListTasksResponse> List(const ListTasksRequest& request) const = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                          const lf::a2a::v1::Message& message,
                                                                          HistoryAppendPolicy policy) = 0;
  [[nodiscard]] virtual HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const = 0;
};

struct TaskStoreStringHash final {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
    return (*this)(std::string_view(value));
  }

  [[nodiscard]] std::size_t operator()(const char* value) const noexcept { return (*this)(std::string_view(value)); }
};

struct TaskStoreStringEqual final {
  using is_transparent = void;

  [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept { return lhs == rhs; }
};

class InMemoryTaskStore final : public TaskStore {
 public:
  class HistoryTelemetrySink {
   public:
    virtual ~HistoryTelemetrySink() = default;
    virtual void OnDedupedHistoryMessage(const HistoryDedupeEvent& event) = 0;
  };

  InMemoryTaskStore() = default;
  explicit InMemoryTaskStore(std::shared_ptr<HistoryTelemetrySink> telemetry_sink);

  [[nodiscard]] core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override;
  [[nodiscard]] core::Result<ListTasksResponse> List(const ListTasksRequest& request) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> AppendTaskHistory(std::string_view task_id,
                                                                  const lf::a2a::v1::Message& message,
                                                                  HistoryAppendPolicy policy) override;
  [[nodiscard]] HistoryTelemetrySnapshot GetHistoryTelemetrySnapshot() const override;

 private:
  static std::optional<std::size_t> ParsePageToken(std::string_view token);

  mutable std::shared_mutex mutex_;
  std::shared_ptr<HistoryTelemetrySink> telemetry_sink_;
  HistoryTelemetrySnapshot telemetry_snapshot_;
  std::vector<std::string> ordered_ids_;
  std::unordered_map<std::string, lf::a2a::v1::Task, TaskStoreStringHash, TaskStoreStringEqual> tasks_;
};

class TaskLifecycleService final {
 public:
  explicit TaskLifecycleService(TaskStore* store, std::shared_ptr<TaskIdGenerator> task_id_generator = nullptr);

  [[nodiscard]] core::Result<lf::a2a::v1::Task> CreateOrUpdateTask(const lf::a2a::v1::Task& task) const;
  // Returns an owning string because the id may be newly generated and must outlive this call.
  // Returning string_view would be unsafe for generated values due to temporary lifetime.
  [[nodiscard]] core::Result<std::string> ResolveTaskIdForSendRequest(const lf::a2a::v1::SendMessageRequest& request,
                                                                      const RequestContext& context) const;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> TransitionTaskStatus(std::string_view task_id,
                                                                     lf::a2a::v1::TaskState next_state) const;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> AppendHistory(std::string_view task_id,
                                                              const lf::a2a::v1::Message& message,
                                                              TaskStore::HistoryAppendPolicy policy) const;

 private:
  TaskStore* store_ = nullptr;
  std::shared_ptr<TaskIdGenerator> task_id_generator_;
};

[[nodiscard]] core::Result<std::size_t> ParseListPageToken(std::string_view page_token);
[[nodiscard]] core::Result<void> ValidateListPageOffset(std::size_t offset, std::size_t size);
void ApplyHistoryRetention(lf::a2a::v1::Task* task, std::optional<std::size_t> history_length);
void ApplyArtifactProjection(lf::a2a::v1::Task* task, bool include_artifacts);

class TimestampDescTaskOrdering final {
 public:
  static void Sort(std::vector<const lf::a2a::v1::Task*>* tasks);
};

}  // namespace a2a::server
