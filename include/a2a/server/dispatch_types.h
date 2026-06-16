// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <variant>

#include "a2a/server/server_stream_session.h"
#include "a2a/server/tasks/list_tasks.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

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

}  // namespace a2a::server
