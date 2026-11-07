// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/json_rpc_transport.h"

#include <google/protobuf/empty.pb.h>
#include <google/protobuf/struct.pb.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/json_rpc.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "a2a/http/http_client.h"
#include "json_rpc_transport_internal.h"
#include "request_headers.h"

namespace a2a::client {
namespace {

using json_rpc_internal::BuildJsonRpcEnvelope;
using json_rpc_internal::BuildJsonRpcEnvelopeError;
using json_rpc_internal::JoinUrl;
using json_rpc_internal::ParseListTasksResult;
using json_rpc_internal::ParseResponseResult;
using json_rpc_internal::ParseResultMessage;
using json_rpc_internal::ToClientHttpResponse;
using json_rpc_internal::ToSharedHttpRequest;
using json_rpc_internal::ValidateJsonRpcHttpStatus;
using json_rpc_internal::ValidateResponseVersion;

constexpr const char* kPageSizeField = "pageSize";
constexpr const char* kPageTokenField = "pageToken";
constexpr const char* kEmptyRequestId = "JSON-RPC request id generator returned an empty id";
constexpr const char* kGetTaskIdRequired = "GetTaskRequest.id is required";
constexpr const char* kCancelTaskIdRequired = "CancelTaskRequest.id is required";
constexpr const char* kGetPushConfigIdRequired = "GetTaskPushNotificationConfigRequest.id is required";
constexpr const char* kDeletePushConfigIdRequired = "DeleteTaskPushNotificationConfigRequest.id is required";
constexpr const char* kMissingRequester = "HTTP requester is not configured";
constexpr std::string_view kDefaultMtlsUnsupportedMessage =
    "default libcurl HTTP requester does not support mTLS options; inject a custom requester for mTLS";

HttpRequester MakeHttpRequesterForClient(std::shared_ptr<a2a::http::Client> client) {
  return [client = std::move(client)](const HttpRequest& request) -> core::Result<HttpClientResponse> {
    if (request.mtls.has_value()) {
      return core::Error::Validation(std::string(kDefaultMtlsUnsupportedMessage));
    }
    auto response = client->SendRequest(ToSharedHttpRequest(request));
    if (!response.ok()) {
      return response.error();
    }
    return ToClientHttpResponse(std::move(response.value()));
  };
}

std::string BuildDefaultRequestId() {
  static std::atomic<std::uint64_t> sequence{0};
  const auto current = sequence.fetch_add(1, std::memory_order_relaxed);
  return "jsonrpc-" + std::to_string(current);
}

}  // namespace

JsonRpcTransport::JsonRpcTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                   std::chrono::milliseconds default_timeout, RequestIdGenerator id_generator)
    : JsonRpcTransport(std::move(resolved_interface), std::move(requester), HttpStreamRequester{}, default_timeout,
                       std::move(id_generator)) {}

JsonRpcTransport::JsonRpcTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                   HttpStreamRequester stream_requester, std::chrono::milliseconds default_timeout,
                                   RequestIdGenerator id_generator, HttpAsyncRequester async_requester)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      stream_requester_(std::move(stream_requester)),
      default_timeout_(default_timeout),
      id_generator_(std::make_shared<RequestIdGenerator>(id_generator ? std::move(id_generator)
                                                                      : RequestIdGenerator(BuildDefaultRequestId))) {
  async_transport_ = json_rpc_internal::MakeAsyncTransport(resolved_interface_, std::move(async_requester),
                                                           default_timeout_, id_generator_);
}

std::unique_ptr<JsonRpcTransport> JsonRpcTransport::CreateDefault(ResolvedInterface resolved_interface,
                                                                  std::chrono::milliseconds default_timeout,
                                                                  RequestIdGenerator id_generator) {
  auto default_http_client = std::make_shared<a2a::http::Client>();
  auto async_requester = [client = default_http_client](
                             const HttpRequest& request,
                             core::Completion<HttpClientResponse> completion) -> core::Result<core::AsyncHandle> {
    if (request.mtls) {
      return core::Error::Validation(std::string(kDefaultMtlsUnsupportedMessage));
    }
    return client->SendRequestAsync(ToSharedHttpRequest(request),
                                    [completion = std::move(completion)](core::Result<http::Response> response) {
                                      if (!response.ok()) {
                                        completion(response.error());
                                      } else {
                                        completion(ToClientHttpResponse(std::move(response.value())));
                                      }
                                    });
  };
  auto transport = std::make_unique<JsonRpcTransport>(
      std::move(resolved_interface), MakeHttpRequesterForClient(default_http_client), HttpStreamRequester{},
      default_timeout, std::move(id_generator), std::move(async_requester));
  transport->default_async_stream_client_ = std::move(default_http_client);
  return transport;
}

core::Result<HttpRequest> JsonRpcTransport::BuildUnaryRequest(std::string request_body,
                                                              const CallOptions& options) const {
  return json_rpc_internal::BuildUnaryRequest(resolved_interface_, default_timeout_, std::move(request_body), options);
}

core::Result<HttpClientResponse> JsonRpcTransport::SendJsonRpcRequest(std::string request_body,
                                                                      const CallOptions& options) const {
  if (!requester_) {
    return core::Error::Internal(kMissingRequester);
  }
  const auto prepared = BuildUnaryRequest(std::move(request_body), options);
  if (!prepared.ok()) {
    return prepared.error();
  }
  const auto response = requester_(prepared.value());
  if (!response.ok()) {
    return response.error();
  }

  const auto version_check = ValidateResponseVersion(response.value());
  if (!version_check.ok()) {
    return version_check.error();
  }
  return response.value();
}

core::Result<std::string> JsonRpcTransport::InvokeForResultJson(std::string_view method_name,
                                                                const google::protobuf::Message& request,
                                                                const CallOptions& options) const {
  if (method_name.empty()) {
    return core::Error::Validation("JSON-RPC method name is required");
  }

  const std::string request_id = (*id_generator_)();
  if (request_id.empty()) {
    return core::Error::Internal(kEmptyRequestId);
  }

  const auto envelope_json = BuildJsonRpcEnvelope(method_name, request, request_id);
  if (!envelope_json.ok()) {
    return envelope_json.error();
  }

  const auto response = SendJsonRpcRequest(envelope_json.value(), options);
  if (!response.ok()) {
    return response.error();
  }

  const auto result = ParseResponseResult(response.value(), request_id);
  if (!result.ok()) {
    return result.error();
  }

  const auto status = ValidateJsonRpcHttpStatus(response.value());
  if (!status.ok()) {
    return status.error();
  }

  return result.value();
}

core::Result<lf::a2a::v1::SendMessageResponse> JsonRpcTransport::SendMessage(
    const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) {
  const auto result = InvokeForResultJson(core::json_rpc::MethodNames::kSendMessage, request, options);
  if (!result.ok()) {
    return result.error();
  }

  const auto response = ParseResultMessage<lf::a2a::v1::SendMessageResponse>(result.value(), core::http::kStatusOk);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::Task> JsonRpcTransport::GetTask(const lf::a2a::v1::GetTaskRequest& request,
                                                          const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation(kGetTaskIdRequired);
  }

  const auto result = InvokeForResultJson(core::json_rpc::MethodNames::kGetTask, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response = ParseResultMessage<lf::a2a::v1::Task>(result.value(), core::http::kStatusOk);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<ListTasksResponse> JsonRpcTransport::ListTasks(const ListTasksRequest& request,
                                                            const CallOptions& options) {
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
  const auto response = SendJsonRpcRequest(envelope_json.value(), options);
  if (!response.ok()) {
    return response.error();
  }
  auto typed_result = ParseListTasksResult(response.value(), request_id);
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
}

core::Result<lf::a2a::v1::Task> JsonRpcTransport::CancelTask(const lf::a2a::v1::CancelTaskRequest& request,
                                                             const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation(kCancelTaskIdRequired);
  }

  const auto result = InvokeForResultJson(core::json_rpc::MethodNames::kCancelTask, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response = ParseResultMessage<lf::a2a::v1::Task>(result.value(), core::http::kStatusOk);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> JsonRpcTransport::CreateTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  const auto result =
      InvokeForResultJson(core::json_rpc::MethodNames::kCreateTaskPushNotificationConfig, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response =
      ParseResultMessage<lf::a2a::v1::TaskPushNotificationConfig>(result.value(), core::http::kStatusOk);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> JsonRpcTransport::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation(kGetPushConfigIdRequired);
  }

  const auto result =
      InvokeForResultJson(core::json_rpc::MethodNames::kGetTaskPushNotificationConfig, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response =
      ParseResultMessage<lf::a2a::v1::TaskPushNotificationConfig>(result.value(), core::http::kStatusOk);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse> JsonRpcTransport::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request, const CallOptions& options) {
  const auto result =
      InvokeForResultJson(core::json_rpc::MethodNames::kListTaskPushNotificationConfigs, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response =
      ParseResultMessage<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(result.value(), core::http::kStatusOk);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<void> JsonRpcTransport::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation(kDeletePushConfigIdRequired);
  }

  const auto result =
      InvokeForResultJson(core::json_rpc::MethodNames::kDeleteTaskPushNotificationConfig, request, options);
  if (!result.ok()) {
    return result.error();
  }

  const auto parsed_empty = ParseResultMessage<google::protobuf::Empty>(result.value(), core::http::kStatusOk);
  if (!parsed_empty.ok()) {
    return parsed_empty.error();
  }
  return {};
}

}  // namespace a2a::client
