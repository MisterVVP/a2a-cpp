#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "a2a/client/call_options.h"
#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/client/http_json_transport.h"
#include "a2a/core/result.h"

namespace a2a::client {

using RequestIdGenerator = std::function<std::string()>;

class JsonRpcTransport final : public ClientTransport {
 public:
  static constexpr std::chrono::milliseconds kDefaultTimeout{30000};

  explicit JsonRpcTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                            std::chrono::milliseconds default_timeout = kDefaultTimeout,
                            RequestIdGenerator id_generator = {});

  [[nodiscard]] core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                        const CallOptions& options) override;
  [[nodiscard]] core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                          const CallOptions& options) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> CancelTask(
      const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> SetTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request,
      const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
  ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
      const CallOptions& options) override;

  [[nodiscard]] core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
      const CallOptions& options) override;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
      const CallOptions& options) override;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SubscribeTask(
      const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer,
      const CallOptions& options) override;

 private:
  [[nodiscard]] core::Result<HttpClientResponse> SendJsonRpcRequest(
      std::string request_body, const CallOptions& options) const;

  [[nodiscard]] core::Result<google::protobuf::Value> InvokeForResultValue(
      std::string_view method_name, const google::protobuf::Message& request,
      const CallOptions& options) const;

  ResolvedInterface resolved_interface_;
  HttpRequester requester_;
  std::chrono::milliseconds default_timeout_;
  RequestIdGenerator id_generator_;
};

}  // namespace a2a::client
