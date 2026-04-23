#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

struct RequestContext final {
  std::optional<std::string> request_id;
  std::optional<std::string> remote_address;
  std::unordered_map<std::string, std::string> auth_metadata;
  std::unordered_map<std::string, std::string> client_headers;
};

struct ListTasksRequest final {
  std::size_t page_size = 0;
  std::string page_token;
};

struct ListTasksResponse final {
  std::vector<lf::a2a::v1::Task> tasks;
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

  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> GetTask(
      const lf::a2a::v1::GetTaskRequest& request, RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                                  RequestContext& context) = 0;

  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> CancelTask(
      const lf::a2a::v1::CancelTaskRequest& request, RequestContext& context) = 0;
};

enum class DispatcherOperation : std::uint8_t {
  kSendMessage,
  kSendStreamingMessage,
  kGetTask,
  kListTasks,
  kCancelTask,
};

struct DispatchRequest final {
  DispatcherOperation operation = DispatcherOperation::kSendMessage;
  std::variant<lf::a2a::v1::SendMessageRequest, lf::a2a::v1::GetTaskRequest, ListTasksRequest,
               lf::a2a::v1::CancelTaskRequest>
      payload;
};

using DispatchPayload =
    std::variant<lf::a2a::v1::SendMessageResponse, std::unique_ptr<ServerStreamSession>,
                 lf::a2a::v1::Task, ListTasksResponse>;

class DispatchResponse final {
 public:
  explicit DispatchResponse(const lf::a2a::v1::SendMessageResponse& payload) : payload_(payload) {}
  explicit DispatchResponse(lf::a2a::v1::SendMessageResponse&& payload)
      : payload_(std::move(payload)) {}
  explicit DispatchResponse(std::unique_ptr<ServerStreamSession> payload)
      : payload_(std::move(payload)) {}
  explicit DispatchResponse(const lf::a2a::v1::Task& payload) : payload_(payload) {}
  explicit DispatchResponse(lf::a2a::v1::Task&& payload) : payload_(std::move(payload)) {}
  explicit DispatchResponse(const ListTasksResponse& payload) : payload_(payload) {}
  explicit DispatchResponse(ListTasksResponse&& payload) : payload_(std::move(payload)) {}

  [[nodiscard]] const DispatchPayload& payload() const noexcept { return payload_; }
  [[nodiscard]] DispatchPayload& payload() noexcept { return payload_; }

 private:
  DispatchPayload payload_;
};

class Dispatcher final {
 public:
  explicit Dispatcher(AgentExecutor* executor);

  [[nodiscard]] core::Result<DispatchResponse> Dispatch(const DispatchRequest& request,
                                                        RequestContext& context) const;

 private:
  AgentExecutor* executor_ = nullptr;
};

class TaskStore {
 public:
  virtual ~TaskStore() = default;

  [[nodiscard]] virtual core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> Get(std::string_view id) const = 0;
  [[nodiscard]] virtual core::Result<ListTasksResponse> List(
      const ListTasksRequest& request) const = 0;
  [[nodiscard]] virtual core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) = 0;
};

class InMemoryTaskStore final : public TaskStore {
 public:
  [[nodiscard]] core::Result<void> CreateOrUpdate(const lf::a2a::v1::Task& task) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Get(std::string_view id) const override;
  [[nodiscard]] core::Result<ListTasksResponse> List(
      const ListTasksRequest& request) const override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> Cancel(std::string_view id) override;

 private:
  static std::optional<std::size_t> ParsePageToken(std::string_view token);

  mutable std::mutex mutex_;
  std::vector<std::string> ordered_ids_;
  std::unordered_map<std::string, lf::a2a::v1::Task> tasks_;
};

struct RestPipelineResponse final {
  int status_code = 0;
  std::unordered_map<std::string, std::string> headers;
  std::optional<DispatchResponse> payload;
  std::optional<core::Error> error;
};

class RestAdapter final {
 public:
  explicit RestAdapter(const Dispatcher* dispatcher);

  [[nodiscard]] RestPipelineResponse Handle(
      const DispatchRequest& request,
      const std::unordered_map<std::string, std::string>& request_headers,
      RequestContext& context) const;

 private:
  const Dispatcher* dispatcher_ = nullptr;
};

}  // namespace a2a::server
