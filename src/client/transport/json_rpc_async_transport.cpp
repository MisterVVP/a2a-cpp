// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include <google/protobuf/empty.pb.h>
#include <google/protobuf/struct.pb.h>

#include <utility>

#include "a2a/client/async_transport.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/json_rpc.h"
#include "json_rpc_transport_internal.h"
namespace a2a::client {
namespace {
using json_rpc_internal::BuildJsonRpcEnvelope;
using json_rpc_internal::ParseListTasksResult;
using json_rpc_internal::ParseResponseResult;
using json_rpc_internal::ParseResultMessage;
using json_rpc_internal::ValidateJsonRpcHttpStatus;
using json_rpc_internal::ValidateResponseVersion;
template <typename T>
struct PreparedRpc final {
  std::string body;
  std::function<core::Result<T>(const HttpClientResponse&)> decode;
};
constexpr const char* kPageSizeField = "pageSize";
constexpr const char* kPageTokenField = "pageToken";
constexpr const char* kEmptyRequestId = "JSON-RPC request id generator returned an empty id";
constexpr const char* kGetTaskIdRequired = "GetTaskRequest.id is required";
constexpr const char* kCancelTaskIdRequired = "CancelTaskRequest.id is required";
constexpr const char* kGetPushConfigIdRequired = "GetTaskPushNotificationConfigRequest.id is required";
constexpr const char* kDeletePushConfigIdRequired = "DeleteTaskPushNotificationConfigRequest.id is required";
constexpr const char* kMissingAsyncRequester = "HTTP async requester is not configured";

class JsonRpcAsyncTransport final : public AsyncClientTransport {
 public:
  JsonRpcAsyncTransport(ResolvedInterface resolved_interface, HttpAsyncRequester requester,
                        std::chrono::milliseconds timeout, std::shared_ptr<RequestIdGenerator> generator)
      : resolved_interface_(std::move(resolved_interface)),
        async_requester_(std::move(requester)),
        default_timeout_(timeout),
        id_generator_(std::move(generator)) {}
  ~JsonRpcAsyncTransport() override { Shutdown(); }

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
  void Execute(core::Result<PreparedRpc<T>> prepared, const CallOptions& options,
               const core::AsyncCompletion<T>& completion) {
    if (!prepared.ok()) {
      completion.Complete(prepared.error());
      return;
    }
    auto plan = std::move(prepared.value());
    auto http_request = BuildUnaryRequest(std::move(plan.body), options);
    if (!http_request.ok()) {
      completion.Complete(http_request.error());
      return;
    }
    if (!async_requester_) {
      completion.Complete(core::Error::Validation(kMissingAsyncRequester));
      return;
    }
    auto started = async_requester_(
        http_request.value(), [completion, decode = std::move(plan.decode)](core::Result<HttpClientResponse> response) {
          if (!response.ok()) {
            completion.Complete(response.error());
            return;
          }
          const auto version = ValidateResponseVersion(response.value());
          if (!version.ok()) {
            completion.Complete(version.error());
            return;
          }
          completion.Complete(decode(response.value()));
        });
    if (!started.ok()) {
      completion.Complete(started.error());
      return;
    }
    completion.RegisterCancellation([handle = std::move(started.value())] { handle.Cancel(); });
  }

  core::Result<HttpRequest> BuildUnaryRequest(std::string request_body, const CallOptions& options) const {
    return json_rpc_internal::BuildUnaryRequest(resolved_interface_, default_timeout_, std::move(request_body),
                                                options);
  }
  ResolvedInterface resolved_interface_;
  HttpAsyncRequester async_requester_;
  std::chrono::milliseconds default_timeout_;
  std::shared_ptr<RequestIdGenerator> id_generator_;
};
void JsonRpcAsyncTransport::StartSendMessage(
    const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::SendMessageResponse>& completion) {
  auto prepare = [&]() -> core::Result<PreparedRpc<lf::a2a::v1::SendMessageResponse>> {
    const std::string request_id = (*id_generator_)();
    if (request_id.empty()) {
      return core::Error::Internal(kEmptyRequestId);
    }
    auto envelope_json = BuildJsonRpcEnvelope(core::json_rpc::MethodNames::kSendMessage, request, request_id);
    if (!envelope_json.ok()) {
      return envelope_json.error();
    }

    return PreparedRpc<lf::a2a::v1::SendMessageResponse>{
        .body = envelope_json.value(),
        .decode =
            [request_id](const HttpClientResponse& input_response) -> core::Result<lf::a2a::v1::SendMessageResponse> {
          const auto parsed_result = ParseResponseResult(input_response, request_id);
          if (!parsed_result.ok()) {
            return parsed_result.error();
          }
          const auto status = ValidateJsonRpcHttpStatus(input_response);
          if (!status.ok()) {
            return status.error();
          }
          const auto& result = parsed_result.value();

          const auto response = ParseResultMessage<lf::a2a::v1::SendMessageResponse>(result, core::http::kStatusOk);
          if (!response.ok()) {
            return response.error();
          }
          return response.value();
        }};
  };
  Execute(prepare(), options, completion);
}
void JsonRpcAsyncTransport::StartGetTask(const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options,
                                         const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  auto prepare = [&]() -> core::Result<PreparedRpc<lf::a2a::v1::Task>> {
    if (request.id().empty()) {
      return core::Error::Validation(kGetTaskIdRequired);
    }

    const std::string request_id = (*id_generator_)();
    if (request_id.empty()) {
      return core::Error::Internal(kEmptyRequestId);
    }
    auto envelope_json = BuildJsonRpcEnvelope(core::json_rpc::MethodNames::kGetTask, request, request_id);
    if (!envelope_json.ok()) {
      return envelope_json.error();
    }

    return PreparedRpc<lf::a2a::v1::Task>{
        .body = envelope_json.value(),
        .decode = [request_id](const HttpClientResponse& input_response) -> core::Result<lf::a2a::v1::Task> {
          const auto parsed_result = ParseResponseResult(input_response, request_id);
          if (!parsed_result.ok()) {
            return parsed_result.error();
          }
          const auto status = ValidateJsonRpcHttpStatus(input_response);
          if (!status.ok()) {
            return status.error();
          }
          const auto& result = parsed_result.value();
          const auto response = ParseResultMessage<lf::a2a::v1::Task>(result, core::http::kStatusOk);
          if (!response.ok()) {
            return response.error();
          }
          return response.value();
        }};
  };
  Execute(prepare(), options, completion);
}
void JsonRpcAsyncTransport::StartListTasks(const ListTasksRequest& request, const CallOptions& options,
                                           const core::AsyncCompletion<ListTasksResponse>& completion) {
  auto prepare = [&]() -> core::Result<PreparedRpc<ListTasksResponse>> {
    google::protobuf::Struct params;
    if (request.page_size > 0) {
      (*params.mutable_fields())[kPageSizeField].set_number_value(static_cast<double>(request.page_size));
    }
    if (!request.page_token.empty()) {
      (*params.mutable_fields())[kPageTokenField].set_string_value(request.page_token);
    }

    const std::string request_id = (*id_generator_)();
    if (request_id.empty()) {
      return core::Error::Internal(kEmptyRequestId);
    }
    const auto envelope_json = BuildJsonRpcEnvelope(core::json_rpc::MethodNames::kListTasks, params, request_id);
    if (!envelope_json.ok()) {
      return envelope_json.error();
    }

    return PreparedRpc<ListTasksResponse>{
        .body = envelope_json.value(),
        .decode = [request_id](const HttpClientResponse& input_response) -> core::Result<ListTasksResponse> {
          auto typed_result = ParseListTasksResult(input_response, request_id);
          if (!typed_result.ok()) {
            return typed_result.error();
          }

          ListTasksResponse parsed;
          auto payload = std::move(typed_result.value());
          parsed.tasks.reserve(static_cast<std::size_t>(payload.tasks_size()));
          for (auto& task : *payload.mutable_tasks()) {
            parsed.tasks.push_back(std::move(task));
          }
          parsed.next_page_token = std::move(*payload.mutable_next_page_token());

          return parsed;
        }};
  };
  Execute(prepare(), options, completion);
}
void JsonRpcAsyncTransport::StartCancelTask(const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options,
                                            const core::AsyncCompletion<lf::a2a::v1::Task>& completion) {
  auto prepare = [&]() -> core::Result<PreparedRpc<lf::a2a::v1::Task>> {
    if (request.id().empty()) {
      return core::Error::Validation(kCancelTaskIdRequired);
    }

    const std::string request_id = (*id_generator_)();
    if (request_id.empty()) {
      return core::Error::Internal(kEmptyRequestId);
    }
    auto envelope_json = BuildJsonRpcEnvelope(core::json_rpc::MethodNames::kCancelTask, request, request_id);
    if (!envelope_json.ok()) {
      return envelope_json.error();
    }

    return PreparedRpc<lf::a2a::v1::Task>{
        .body = envelope_json.value(),
        .decode = [request_id](const HttpClientResponse& input_response) -> core::Result<lf::a2a::v1::Task> {
          const auto parsed_result = ParseResponseResult(input_response, request_id);
          if (!parsed_result.ok()) {
            return parsed_result.error();
          }
          const auto status = ValidateJsonRpcHttpStatus(input_response);
          if (!status.ok()) {
            return status.error();
          }
          const auto& result = parsed_result.value();
          const auto response = ParseResultMessage<lf::a2a::v1::Task>(result, core::http::kStatusOk);
          if (!response.ok()) {
            return response.error();
          }
          return response.value();
        }};
  };
  Execute(prepare(), options, completion);
}
void JsonRpcAsyncTransport::StartCreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  auto prepare = [&]() -> core::Result<PreparedRpc<lf::a2a::v1::TaskPushNotificationConfig>> {
    const std::string request_id = (*id_generator_)();
    if (request_id.empty()) {
      return core::Error::Internal(kEmptyRequestId);
    }
    auto envelope_json =
        BuildJsonRpcEnvelope(core::json_rpc::MethodNames::kCreateTaskPushNotificationConfig, request, request_id);
    if (!envelope_json.ok()) {
      return envelope_json.error();
    }

    return PreparedRpc<lf::a2a::v1::TaskPushNotificationConfig>{
        .body = envelope_json.value(),
        .decode = [request_id](const HttpClientResponse& input_response)
            -> core::Result<lf::a2a::v1::TaskPushNotificationConfig> {
          const auto parsed_result = ParseResponseResult(input_response, request_id);
          if (!parsed_result.ok()) {
            return parsed_result.error();
          }
          const auto status = ValidateJsonRpcHttpStatus(input_response);
          if (!status.ok()) {
            return status.error();
          }
          const auto& result = parsed_result.value();
          const auto response =
              ParseResultMessage<lf::a2a::v1::TaskPushNotificationConfig>(result, core::http::kStatusOk);
          if (!response.ok()) {
            return response.error();
          }
          return response.value();
        }};
  };
  Execute(prepare(), options, completion);
}
void JsonRpcAsyncTransport::StartGetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::TaskPushNotificationConfig>& completion) {
  auto prepare = [&]() -> core::Result<PreparedRpc<lf::a2a::v1::TaskPushNotificationConfig>> {
    if (request.id().empty()) {
      return core::Error::Validation(kGetPushConfigIdRequired);
    }

    const std::string request_id = (*id_generator_)();
    if (request_id.empty()) {
      return core::Error::Internal(kEmptyRequestId);
    }
    auto envelope_json =
        BuildJsonRpcEnvelope(core::json_rpc::MethodNames::kGetTaskPushNotificationConfig, request, request_id);
    if (!envelope_json.ok()) {
      return envelope_json.error();
    }

    return PreparedRpc<lf::a2a::v1::TaskPushNotificationConfig>{
        .body = envelope_json.value(),
        .decode = [request_id](const HttpClientResponse& input_response)
            -> core::Result<lf::a2a::v1::TaskPushNotificationConfig> {
          const auto parsed_result = ParseResponseResult(input_response, request_id);
          if (!parsed_result.ok()) {
            return parsed_result.error();
          }
          const auto status = ValidateJsonRpcHttpStatus(input_response);
          if (!status.ok()) {
            return status.error();
          }
          const auto& result = parsed_result.value();
          const auto response =
              ParseResultMessage<lf::a2a::v1::TaskPushNotificationConfig>(result, core::http::kStatusOk);
          if (!response.ok()) {
            return response.error();
          }
          return response.value();
        }};
  };
  Execute(prepare(), options, completion);
}
void JsonRpcAsyncTransport::StartListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options,
    const core::AsyncCompletion<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>& completion) {
  auto prepare = [&]() -> core::Result<PreparedRpc<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>> {
    const std::string request_id = (*id_generator_)();
    if (request_id.empty()) {
      return core::Error::Internal(kEmptyRequestId);
    }
    auto envelope_json =
        BuildJsonRpcEnvelope(core::json_rpc::MethodNames::kListTaskPushNotificationConfigs, request, request_id);
    if (!envelope_json.ok()) {
      return envelope_json.error();
    }

    return PreparedRpc<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>{
        .body = envelope_json.value(),
        .decode = [request_id](const HttpClientResponse& input_response)
            -> core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> {
          const auto parsed_result = ParseResponseResult(input_response, request_id);
          if (!parsed_result.ok()) {
            return parsed_result.error();
          }
          const auto status = ValidateJsonRpcHttpStatus(input_response);
          if (!status.ok()) {
            return status.error();
          }
          const auto& result = parsed_result.value();
          const auto response =
              ParseResultMessage<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(result, core::http::kStatusOk);
          if (!response.ok()) {
            return response.error();
          }
          return response.value();
        }};
  };
  Execute(prepare(), options, completion);
}
void JsonRpcAsyncTransport::StartDeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options,
    const core::AsyncCompletion<void>& completion) {
  auto prepare = [&]() -> core::Result<PreparedRpc<void>> {
    if (request.id().empty()) {
      return core::Error::Validation(kDeletePushConfigIdRequired);
    }

    const std::string request_id = (*id_generator_)();
    if (request_id.empty()) {
      return core::Error::Internal(kEmptyRequestId);
    }
    auto envelope_json =
        BuildJsonRpcEnvelope(core::json_rpc::MethodNames::kDeleteTaskPushNotificationConfig, request, request_id);
    if (!envelope_json.ok()) {
      return envelope_json.error();
    }

    return PreparedRpc<void>{.body = envelope_json.value(),
                             .decode = [request_id](const HttpClientResponse& input_response) -> core::Result<void> {
                               const auto parsed_result = ParseResponseResult(input_response, request_id);
                               if (!parsed_result.ok()) {
                                 return parsed_result.error();
                               }
                               const auto status = ValidateJsonRpcHttpStatus(input_response);
                               if (!status.ok()) {
                                 return status.error();
                               }
                               const auto& result = parsed_result.value();

                               const auto parsed_empty =
                                   ParseResultMessage<google::protobuf::Empty>(result, core::http::kStatusOk);
                               if (!parsed_empty.ok()) {
                                 return parsed_empty.error();
                               }
                               return {};
                             }};
  };
  Execute(prepare(), options, completion);
}
}  // namespace
namespace json_rpc_internal {
std::shared_ptr<AsyncClientTransport> MakeAsyncTransport(ResolvedInterface resolved_interface,
                                                         HttpAsyncRequester requester,
                                                         std::chrono::milliseconds default_timeout,
                                                         std::shared_ptr<RequestIdGenerator> id_generator) {
  return std::make_shared<JsonRpcAsyncTransport>(std::move(resolved_interface), std::move(requester), default_timeout,
                                                 std::move(id_generator));
}
}  // namespace json_rpc_internal
}  // namespace a2a::client
