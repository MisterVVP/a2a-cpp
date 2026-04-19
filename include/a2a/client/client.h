#pragma once

#include <memory>

#include "a2a/client/call_options.h"
#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::client {

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
  ListTaskPushNotificationConfigs(const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
                                  const CallOptions& options) = 0;

  [[nodiscard]] virtual core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const CallOptions& options) = 0;
};

class A2AClient final {
 public:
  explicit A2AClient(std::unique_ptr<ClientTransport> transport);

  [[nodiscard]] core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::Task> GetTask(
      const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::Task> CancelTask(
      const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> SetTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      const CallOptions& options = {});

  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
                                  const CallOptions& options = {});

  [[nodiscard]] core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const CallOptions& options = {});

 private:
  std::unique_ptr<ClientTransport> transport_;
};

}  // namespace a2a::client
