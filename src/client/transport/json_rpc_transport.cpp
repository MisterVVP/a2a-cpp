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
#include "a2a/core/json_value.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "a2a/http/http_client.h"
#include "json_rpc_transport_internal.h"

namespace a2a::client {
namespace {

using json_rpc_internal::BuildJsonRpcEnvelope;
using json_rpc_internal::BuildJsonRpcEnvelopeError;
using json_rpc_internal::JoinUrl;
using json_rpc_internal::ParseResponseResult;
using json_rpc_internal::ParseResultMessage;
using json_rpc_internal::ToClientHttpResponse;
using json_rpc_internal::ToSharedHttpRequest;
using json_rpc_internal::ValidateResponseVersion;

constexpr std::string_view kEmptyJsonObject = "{}";
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

core::Result<void> ValidateJsonRpcHttpStatus(const HttpClientResponse& response) {
  if (response.status_code >= core::http::kSuccessStatusMin && response.status_code <= core::http::kSuccessStatusMax) {
    return {};
  }
  return BuildJsonRpcEnvelopeError("JSON-RPC response received with non-success HTTP status", response);
}

core::Result<lf::a2a::v1::ListTasksResponse> ParseListTasksResult(const HttpClientResponse& response,
                                                                  std::string_view expected_id) {
  const std::string_view response_body = response.body;
  const auto range = core::json::FindTopLevelObjectMemberValue(response_body, core::json_rpc::kResultMemberName);
  if (!range.has_value()) {
    const auto result = ParseResponseResult(response, expected_id);
    if (!result.ok()) {
      return result.error();
    }
    const auto status = ValidateJsonRpcHttpStatus(response);
    if (!status.ok()) {
      return status.error();
    }
    if (result.value().kind_case() != google::protobuf::Value::kStructValue) {
      return core::Error::Serialization("ListTasks JSON-RPC result must be an object")
          .WithTransport("jsonrpc")
          .WithHttpStatus(response.status_code);
    }
    const auto result_json = core::MessageToJson(result.value());
    if (!result_json.ok()) {
      return result_json.error();
    }
    lf::a2a::v1::ListTasksResponse decoded_result;
    const auto parsed = core::JsonToMessage(result_json.value(), &decoded_result,
                                            {.ignore_unknown_fields = true,
                                             .reject_top_level_null_fields = true,
                                             .reject_duplicate_top_level_fields = true});
    if (!parsed.ok()) {
      return parsed.error().WithTransport("jsonrpc").WithHttpStatus(response.status_code);
    }
    return decoded_result;
  }
  const std::string_view prefix = response_body.substr(0, range->begin);
  const std::string_view suffix = response_body.substr(range->end);
  std::string validation_body;
  validation_body.reserve(prefix.size() + suffix.size() + kEmptyJsonObject.size());
  validation_body.append(prefix);
  validation_body.append(kEmptyJsonObject);
  validation_body.append(suffix);
  const HttpClientResponse validation_response{
      .status_code = response.status_code,
      .headers = {},
      .body = std::move(validation_body),
  };
  const auto validated = ParseResponseResult(validation_response, expected_id);
  if (!validated.ok()) {
    return validated.error();
  }
  const auto status = ValidateJsonRpcHttpStatus(response);
  if (!status.ok()) {
    return status.error();
  }
  lf::a2a::v1::ListTasksResponse result;
  const std::string_view result_json = response_body.substr(range->begin, range->end - range->begin);
  const auto parsed = core::JsonToMessage(
      result_json, &result,
      {.ignore_unknown_fields = true, .reject_top_level_null_fields = true, .reject_duplicate_top_level_fields = true});
  if (!parsed.ok()) {
    return parsed.error().WithTransport("jsonrpc").WithHttpStatus(response.status_code);
  }
  return result;
}

}  // namespace

JsonRpcTransport::JsonRpcTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                   std::chrono::milliseconds default_timeout, RequestIdGenerator id_generator)
    : JsonRpcTransport(std::move(resolved_interface), std::move(requester), HttpStreamRequester{}, default_timeout,
                       std::move(id_generator)) {}

JsonRpcTransport::JsonRpcTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                   HttpStreamRequester stream_requester, std::chrono::milliseconds default_timeout,
                                   RequestIdGenerator id_generator)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      stream_requester_(std::move(stream_requester)),
      default_timeout_(default_timeout),
      id_generator_(std::move(id_generator)) {
  if (id_generator_ == nullptr) {
    id_generator_ = BuildDefaultRequestId;
  }
}

std::unique_ptr<JsonRpcTransport> JsonRpcTransport::CreateDefault(ResolvedInterface resolved_interface,
                                                                  std::chrono::milliseconds default_timeout,
                                                                  RequestIdGenerator id_generator) {
  auto default_http_client = std::make_shared<a2a::http::Client>();
  auto transport =
      std::make_unique<JsonRpcTransport>(std::move(resolved_interface), MakeHttpRequesterForClient(default_http_client),
                                         HttpStreamRequester{}, default_timeout, std::move(id_generator));
  transport->default_async_stream_client_ = std::move(default_http_client);
  return transport;
}

core::Result<HttpClientResponse> JsonRpcTransport::SendJsonRpcRequest(std::string request_body,
                                                                      const CallOptions& options) const {
  if (resolved_interface_.transport != PreferredTransport::kJsonRpc) {
    return core::Error::Validation("JsonRpcTransport requires a JSON-RPC interface");
  }
  if (resolved_interface_.url.empty()) {
    return core::Error::Validation("Resolved JSON-RPC interface URL is required");
  }
  if (requester_ == nullptr) {
    return core::Error::Internal("HTTP requester is not configured");
  }

  HttpRequest http_request;
  http_request.method = std::string(core::http::kMethodPost);
  http_request.url = JoinUrl(resolved_interface_.url);
  http_request.body = std::move(request_body);
  http_request.timeout = options.timeout.value_or(default_timeout_);
  http_request.headers = options.headers;
  http_request.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  http_request.headers[std::string(core::http::kContentTypeHeaderName)] =
      std::string(core::http::kContentTypeApplicationJson);
  http_request.headers[std::string(core::http::kAcceptHeaderName)] =
      std::string(core::http::kContentTypeApplicationJson);
  http_request.mtls = options.mtls;

  if (!options.extensions.empty()) {
    http_request.headers[std::string(core::Extensions::kHeaderName)] = core::Extensions::Format(options.extensions);
  }
  if (options.auth_hook) {
    options.auth_hook(http_request.headers);
  }
  if (options.credential_provider != nullptr) {
    const auto applied =
        ApplyCredentialProvider(*options.credential_provider, options.auth_context, &http_request.headers);
    if (!applied.ok()) {
      return applied.error();
    }
  }

  const auto response = requester_(http_request);
  if (!response.ok()) {
    return response.error();
  }

  const auto version_check = ValidateResponseVersion(response.value());
  if (!version_check.ok()) {
    return version_check.error();
  }
  return response.value();
}

core::Result<google::protobuf::Value> JsonRpcTransport::InvokeForResultValue(std::string_view method_name,
                                                                             const google::protobuf::Message& request,
                                                                             const CallOptions& options) const {
  if (method_name.empty()) {
    return core::Error::Validation("JSON-RPC method name is required");
  }

  const std::string request_id = id_generator_();
  if (request_id.empty()) {
    return core::Error::Internal("JSON-RPC request id generator returned an empty id");
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
  const auto result = InvokeForResultValue(core::json_rpc::MethodNames::kSendMessage, request, options);
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
    return core::Error::Validation("GetTaskRequest.id is required");
  }

  const auto result = InvokeForResultValue(core::json_rpc::MethodNames::kGetTask, request, options);
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
    (*params.mutable_fields())["pageSize"].set_number_value(static_cast<double>(request.page_size));
  }
  if (!request.page_token.empty()) {
    (*params.mutable_fields())["pageToken"].set_string_value(request.page_token);
  }

  const std::string request_id = id_generator_();
  if (request_id.empty()) {
    return core::Error::Internal("JSON-RPC request id generator returned an empty id");
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
    return core::Error::Validation("CancelTaskRequest.id is required");
  }

  const auto result = InvokeForResultValue(core::json_rpc::MethodNames::kCancelTask, request, options);
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
      InvokeForResultValue(core::json_rpc::MethodNames::kCreateTaskPushNotificationConfig, request, options);
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
    return core::Error::Validation("GetTaskPushNotificationConfigRequest.id is required");
  }

  const auto result =
      InvokeForResultValue(core::json_rpc::MethodNames::kGetTaskPushNotificationConfig, request, options);
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
      InvokeForResultValue(core::json_rpc::MethodNames::kListTaskPushNotificationConfigs, request, options);
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
    return core::Error::Validation("DeleteTaskPushNotificationConfigRequest.id is required");
  }

  const auto result =
      InvokeForResultValue(core::json_rpc::MethodNames::kDeleteTaskPushNotificationConfig, request, options);
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
