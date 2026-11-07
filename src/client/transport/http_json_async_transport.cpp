// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <google/protobuf/empty.pb.h>
#include <google/protobuf/struct.pb.h>

#include <utility>

#include "a2a/client/async_transport.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/json_rpc.h"
#include "http_json_transport_internal.h"
namespace a2a::client {
namespace {
using http_json_internal::PrepareCancelTask;
using http_json_internal::PrepareCreateTaskPushNotificationConfig;
using http_json_internal::PrepareDeleteTaskPushNotificationConfig;
using http_json_internal::PreparedUnary;
using http_json_internal::PrepareGetTask;
using http_json_internal::PrepareGetTaskPushNotificationConfig;
using http_json_internal::PrepareListTaskPushNotificationConfigs;
using http_json_internal::PrepareListTasks;
using http_json_internal::PrepareSendMessage;
using http_json_internal::ValidateResponseVersion;
constexpr const char* kMissingAsyncRequester = "HTTP async requester is not configured";
class HttpJsonAsyncTransport final : public AsyncClientTransport {
 public:
  HttpJsonAsyncTransport(ResolvedInterface resolved_interface, HttpAsyncRequester requester,
                         std::chrono::milliseconds timeout)
      : resolved_interface_(std::move(resolved_interface)),
        async_requester_(std::move(requester)),
        default_timeout_(timeout) {}
  ~HttpJsonAsyncTransport() override { Shutdown(); }

 protected:
  [[nodiscard]] std::chrono::milliseconds DefaultAsyncTimeout() const override { return default_timeout_; }
  void StartSendMessage(const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
                        const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) override;
  void StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                    const core::AsyncCompletion<lf::a2a::v1::Task>& completion) override;
  void StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                      const core::AsyncCompletion<ListTasksResponse>& completion) override;
  void StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                       const core::AsyncCompletion<lf::a2a::v1::Task>& completion) override;
  void StartCreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) override;
  void StartGetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) override;
  void StartListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
      const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) override;
  void StartDeleteTaskPushNotificationConfig(const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
                                             const CallOptions& options,
                                             const core::AsyncCompletion<void>& completion) override;

 private:
  template <typename T>
  void Execute(core::Result<PreparedUnary<T>> prepared, const CallOptions& options,
               const core::AsyncCompletion<T>& completion) {
    if (!prepared.ok()) {
      completion.Complete(prepared.error());
      return;
    }
    auto plan = std::move(prepared.value());
    auto http_request =
        BuildUnaryRequest({.method = plan.method, .endpoint = plan.endpoint}, std::move(plan.body), options);
    if (!http_request.ok()) {
      completion.Complete(http_request.error());
      return;
    }
    if (!async_requester_) {
      completion.Complete(core::Error::Validation(kMissingAsyncRequester));
      return;
    }
    auto started =
        async_requester_(http_request.value(), [completion, decode = plan.decode, endpoint = std::move(plan.endpoint)](
                                                   core::Result<HttpClientResponse> response) {
          if (!response.ok()) {
            completion.Complete(response.error());
            return;
          }
          const auto version = ValidateResponseVersion(response.value());
          if (!version.ok()) {
            completion.Complete(version.error());
            return;
          }
          completion.Complete(decode(response.value(), endpoint));
        });
    if (!started.ok()) {
      completion.Complete(started.error());
      return;
    }
    completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
  }

  core::Result<HttpRequest> BuildUnaryRequest(HttpOperation operation, std::string body,
                                              const CallOptions& options) const {
    return http_json_internal::BuildUnaryRequest(resolved_interface_, default_timeout_, operation, std::move(body),
                                                 options);
  }
  ResolvedInterface resolved_interface_;
  HttpAsyncRequester async_requester_;
  std::chrono::milliseconds default_timeout_;
};
void HttpJsonAsyncTransport::StartSendMessage(
    const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) {
  Execute(PrepareSendMessage(request), options, completion);
}
void HttpJsonAsyncTransport::StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                                          const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  Execute(PrepareGetTask(request), options, completion);
}
void HttpJsonAsyncTransport::StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                                            const core::AsyncCompletion<ListTasksResponse>& completion) {
  Execute(PrepareListTasks(request), options, completion);
}
void HttpJsonAsyncTransport::StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                                             const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  Execute(PrepareCancelTask(request), options, completion);
}
void HttpJsonAsyncTransport::StartCreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  Execute(PrepareCreateTaskPushNotificationConfig(request), options, completion);
}
void HttpJsonAsyncTransport::StartGetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  Execute(PrepareGetTaskPushNotificationConfig(request), options, completion);
}
void HttpJsonAsyncTransport::StartListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) {
  Execute(PrepareListTaskPushNotificationConfigs(request), options, completion);
}
void HttpJsonAsyncTransport::StartDeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<void>& completion) {
  Execute(PrepareDeleteTaskPushNotificationConfig(request), options, completion);
}
}  // namespace
namespace http_json_internal {
std::shared_ptr<AsyncClientTransport> MakeAsyncTransport(ResolvedInterface resolved_interface,
                                                         HttpAsyncRequester requester,
                                                         std::chrono::milliseconds default_timeout) {
  return std::make_shared<HttpJsonAsyncTransport>(std::move(resolved_interface), std::move(requester), default_timeout);
}
}  // namespace http_json_internal
}  // namespace a2a::client
