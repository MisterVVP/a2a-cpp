// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "a2a/client/call_options.h"
#include "a2a/client/client.h"
#include "a2a/client/discovery.h"
#include "a2a/core/result.h"

namespace a2a::http {
class Client;
}

namespace a2a::client {

struct HttpRequest final {
  std::string method;
  std::string url;
  HeaderMap headers;
  std::string body;
  std::chrono::milliseconds timeout{0};
  std::optional<http::TlsOptions> tls = std::nullopt;
  std::optional<MtlsConfig> mtls = std::nullopt;
};

struct HttpClientResponse final {
  int status_code = 0;
  HeaderMap headers;
  std::string body;
};

using HttpAsyncRequester = std::function<core::Result<core::AsyncHandle>(
    const HttpRequest& request, core::Completion<HttpClientResponse> completion)>;
using HttpRequester = std::function<core::Result<HttpClientResponse>(const HttpRequest& request)>;
using HttpStreamMetadataHandler = std::function<core::Result<void>(const HttpClientResponse& response)>;
using HttpStreamChunkHandler = std::function<core::Result<void>(std::string_view chunk)>;
using StreamCancelled = std::function<bool()>;
using StreamCancellationRegistrar = std::function<void(const std::function<void()>&)>;
using HttpStreamRequester = std::function<core::Result<HttpClientResponse>(
    const HttpRequest& request, const HttpStreamMetadataHandler& on_metadata, const HttpStreamChunkHandler& on_chunk,
    const StreamCancelled& is_cancelled)>;
using HttpStreamRequesterWithCancellation = std::function<core::Result<HttpClientResponse>(
    const HttpRequest& request, const HttpStreamMetadataHandler& on_metadata, const HttpStreamChunkHandler& on_chunk,
    const StreamCancelled& is_cancelled, const StreamCancellationRegistrar& register_cancellation)>;

[[nodiscard]] HttpRequester MakeDefaultHttpRequester();
[[nodiscard]] HttpStreamRequester MakeDefaultHttpStreamRequester();

struct HttpOperation final {
  std::string_view method;
  std::string_view endpoint;
};

class HttpJsonTransport final : public ClientTransport {
 public:
  static constexpr std::chrono::milliseconds kDefaultTimeout{30000};

  explicit HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                             HttpStreamRequester stream_requester,
                             std::chrono::milliseconds default_timeout = kDefaultTimeout,
                             HttpAsyncRequester async_requester = {});

  explicit HttpJsonTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                             std::chrono::milliseconds default_timeout = kDefaultTimeout);

  [[nodiscard]] static std::unique_ptr<HttpJsonTransport> CreateDefault(
      ResolvedInterface resolved_interface, std::chrono::milliseconds default_timeout = kDefaultTimeout);

  [[nodiscard]] core::Result<lf::a2a::v1::SendMessageResponse> SendMessage(
      const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                        const CallOptions& options) override;
  [[nodiscard]] core::Result<ListTasksResponse> ListTasks(const ListTasksRequest& request,
                                                          const CallOptions& options) override;
  [[nodiscard]] core::Result<lf::a2a::v1::Task> CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                           const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> CreateTaskPushNotificationConfig(
      const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::TaskPushNotificationConfig> GetTaskPushNotificationConfig(
      const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> ListTaskPushNotificationConfigs(
      const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<void> DeleteTaskPushNotificationConfig(
      const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) override;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SendStreamingMessage(
      const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) override;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                          StreamObserver& observer,
                                                                          const CallOptions& options) override;
  [[nodiscard]] core::Result<void> Shutdown() override;

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
  [[nodiscard]] core::Result<HttpRequest> BuildUnaryRequest(HttpOperation operation, std::string body,
                                                            const CallOptions& options) const;
  [[nodiscard]] core::Result<HttpClientResponse> SendRequest(HttpOperation operation, std::string body,
                                                             const CallOptions& options) const;

  [[nodiscard]] core::Result<std::unique_ptr<StreamHandle>> StartSseStream(HttpOperation operation, std::string body,
                                                                           StreamObserver& observer,
                                                                           const CallOptions& options) const;

  ResolvedInterface resolved_interface_;
  HttpRequester requester_;
  HttpAsyncRequester async_requester_;
  HttpStreamRequester stream_requester_;
  HttpStreamRequesterWithCancellation cancellable_stream_requester_;
  std::chrono::milliseconds default_timeout_;
  std::shared_ptr<std::mutex> async_client_mutex_ = std::make_shared<std::mutex>();
  std::shared_ptr<http::Client> default_async_stream_client_;
  std::shared_ptr<std::atomic<bool>> async_shutdown_ = std::make_shared<std::atomic<bool>>(false);
};

}  // namespace a2a::client
