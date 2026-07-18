// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/json_rpc_transport.h"

#include <google/protobuf/empty.pb.h>
#include <google/protobuf/struct.pb.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "a2a/client/sse_parser.h"
#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/json_rpc.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::client {
namespace {

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr std::string_view kStreamingSuccessRequiresSseMessage =
    "JSON-RPC streaming success response must use text/event-stream";

std::string JoinUrl(std::string_view interface_base_url) {
  std::string base(interface_base_url);
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base;
}

std::string BuildDefaultRequestId() {
  static std::atomic<std::uint64_t> sequence{0};
  const auto current = sequence.fetch_add(1, std::memory_order_relaxed);
  return "jsonrpc-" + std::to_string(current);
}

core::Result<void> ValidateResponseVersion(const HttpClientResponse& response) {
  const auto version = core::http::FindHeaderValue(response.headers, core::Version::kHeaderName);
  if (!version.has_value()) {
    return {};
  }

  if (!core::Version::IsSupported(version.value())) {
    return core::Error::UnsupportedVersion("Server returned unsupported A2A-Version header")
        .WithTransport("jsonrpc")
        .WithProtocolCode(std::string(version.value()));
  }

  return {};
}

core::Error BuildJsonRpcEnvelopeError(std::string_view message, const HttpClientResponse& response) {
  return core::Error::RemoteProtocol(std::string(message))
      .WithTransport("jsonrpc")
      .WithHttpStatus(response.status_code);
}

core::Error BuildRemoteJsonRpcError(const google::protobuf::Value& error_value, const HttpClientResponse& response) {
  if (!error_value.has_struct_value()) {
    return BuildJsonRpcEnvelopeError("JSON-RPC error payload must be an object", response);
  }

  const auto& fields = error_value.struct_value().fields();
  std::string message = "JSON-RPC request failed";
  std::string code;

  const auto message_it = fields.find("message");
  if (message_it != fields.end() && message_it->second.kind_case() == ::google::protobuf::Value::kStringValue) {
    message = message_it->second.string_value();
  }

  const auto code_it = fields.find("code");
  if (code_it != fields.end()) {
    if (code_it->second.kind_case() == ::google::protobuf::Value::kNumberValue) {
      const auto numeric_code = static_cast<int>(code_it->second.number_value());
      code = std::to_string(numeric_code);
    } else if (code_it->second.kind_case() == ::google::protobuf::Value::kStringValue) {
      code = code_it->second.string_value();
    }
  }

  core::Error error =
      core::Error::RemoteProtocol(message).WithTransport("jsonrpc").WithHttpStatus(response.status_code);
  if (!code.empty()) {
    error = error.WithProtocolCode(code);
  }
  return error;
}

core::Result<google::protobuf::Struct> ParseEnvelope(std::string_view payload, const HttpClientResponse& response) {
  google::protobuf::Struct envelope;
  const auto parse = core::JsonToMessage(payload, &envelope);
  if (!parse.ok()) {
    return parse.error().WithTransport("jsonrpc").WithHttpStatus(response.status_code);
  }
  return envelope;
}

core::Result<google::protobuf::Value> ParseResponseResult(const HttpClientResponse& response,
                                                          std::string_view expected_id) {
  const auto parsed = ParseEnvelope(response.body, response);
  if (!parsed.ok()) {
    return parsed.error();
  }

  const auto& fields = parsed.value().fields();
  const auto version_it = fields.find("jsonrpc");
  if (version_it == fields.end() || version_it->second.kind_case() != ::google::protobuf::Value::kStringValue ||
      version_it->second.string_value() != core::json_rpc::kVersion) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response has invalid version", response);
  }

  const auto id_it = fields.find("id");
  if (id_it == fields.end() || id_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response id must be a string", response);
  }
  if (id_it->second.string_value() != expected_id) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response id does not match request id", response);
  }

  const auto error_it = fields.find("error");
  const auto result_it = fields.find("result");
  if (error_it != fields.end() && result_it != fields.end()) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response must not contain both result and error", response);
  }

  if (error_it != fields.end()) {
    return BuildRemoteJsonRpcError(error_it->second, response);
  }

  if (result_it == fields.end()) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response is missing result", response);
  }

  return result_it->second;
}

template <typename T>
core::Result<T> ParseResultMessage(const google::protobuf::Value& result_value, int response_status_code) {
  T message;
  const auto json = core::MessageToJson(result_value);
  if (!json.ok()) {
    return json.error().WithTransport("jsonrpc").WithHttpStatus(response_status_code);
  }
  const auto parse = core::JsonToMessage(json.value(), &message);
  if (!parse.ok()) {
    return parse.error().WithTransport("jsonrpc").WithHttpStatus(response_status_code);
  }
  return message;
}

bool HasSseContentType(const HeaderMap& headers) {
  const auto content_type = core::http::FindHeaderValue(headers, core::http::kContentTypeHeaderName);
  return content_type.has_value() && core::http::IsSseContentType(content_type.value());
}

bool HasJsonContentType(const HeaderMap& headers) {
  const auto content_type = core::http::FindHeaderValue(headers, core::http::kContentTypeHeaderName);
  return content_type.has_value() && core::http::IsJsonContentType(content_type.value());
}

core::Error BuildJsonRpcStreamStatusError(const HttpClientResponse& response) {
  return core::Error::RemoteProtocol("JSON-RPC stream returned non-success HTTP status")
      .WithTransport("jsonrpc")
      .WithHttpStatus(response.status_code);
}

void MarkInactive(StreamHandle::State& state) { state.active.store(false); }

void NotifyErrorAndStop(StreamHandle::State& state, StreamObserver& observer, const core::Error& error) {
  observer.OnError(error);
  MarkInactive(state);
}

// Shared by unary and SSE JSON-RPC calls so request envelope fields stay consistent.
core::Result<std::string> BuildJsonRpcEnvelope(std::string_view method_name, const google::protobuf::Message& request,
                                               std::string_view request_id) {
  const auto request_json = core::MessageToJson(request);
  if (!request_json.ok()) {
    return request_json.error();
  }
  google::protobuf::Value params;
  const auto parse_params = core::JsonToMessage(request_json.value(), &params);
  if (!parse_params.ok()) {
    return parse_params.error();
  }
  google::protobuf::Struct envelope;
  (*envelope.mutable_fields())["jsonrpc"].set_string_value(std::string(core::json_rpc::kVersion));
  (*envelope.mutable_fields())["id"].set_string_value(std::string(request_id));
  (*envelope.mutable_fields())["method"].set_string_value(std::string(method_name));
  (*envelope.mutable_fields())["params"] = params;
  return core::MessageToJson(envelope);
}

core::Result<void> DispatchJsonRpcSseEvent(const SseEvent& event, std::string_view request_id,
                                           const HttpClientResponse& response, StreamObserver& observer) {
  const HttpClientResponse event_response{
      .status_code = response.status_code, .headers = response.headers, .body = event.data};
  const auto result = ParseResponseResult(event_response, request_id);
  if (!result.ok()) {
    return result.error();
  }
  const auto parsed = ParseResultMessage<lf::a2a::v1::StreamResponse>(result.value(), response.status_code);
  if (!parsed.ok()) {
    return parsed.error();
  }
  observer.OnEvent(parsed.value());
  return {};
}

class JsonRpcSseSession final {
 public:
  JsonRpcSseSession(const HttpStreamRequester& stream_requester, const HttpRequest& http_request,
                    StreamObserver& observer, std::shared_ptr<StreamHandle::State> state, std::string request_id)
      : stream_requester_(stream_requester),
        http_request_(http_request),
        observer_(observer),
        state_(std::move(state)),
        request_id_(std::move(request_id)) {}

  void Run() {
    const auto stream_response = stream_requester_(
        http_request_, [this](const HttpClientResponse& response) { return ValidateMetadata(response); },
        [this](std::string_view chunk) { return HandleChunk(chunk); },
        [this]() { return state_->cancel_requested.load(); });
    if (StopIfCancelled()) {
      return;
    }
    if (!stream_response.ok()) {
      NotifyErrorAndStop(*state_, observer_, stream_response.error());
      return;
    }

    const auto metadata = EnsureMetadata(stream_response.value());
    if (!metadata.ok()) {
      NotifyErrorAndStop(*state_, observer_, metadata.error());
      return;
    }
    if (is_json_response_) {
      HandleJsonResponse(stream_response.value());
      return;
    }

    const auto finish = parser_.Finish([this](const SseEvent& event) { return DispatchEvent(event); });
    if (StopIfCancelled()) {
      return;
    }
    if (!finish.ok()) {
      NotifyErrorAndStop(*state_, observer_, finish.error());
      return;
    }
    observer_.OnCompleted();
    MarkInactive(*state_);
  }

 private:
  core::Result<void> ValidateMetadata(const HttpClientResponse& response) {
    response_metadata_ = response;
    const auto version_check = ValidateResponseVersion(response_metadata_);
    if (!version_check.ok()) {
      return version_check.error();
    }
    if (HasJsonContentType(response_metadata_.headers)) {
      is_json_response_ = true;
      metadata_validated_ = true;
      return {};
    }
    if (response_metadata_.status_code < kHttpOkMin || response_metadata_.status_code > kHttpOkMax) {
      return BuildJsonRpcStreamStatusError(response_metadata_);
    }
    if (HasSseContentType(response_metadata_.headers)) {
      metadata_validated_ = true;
      return {};
    }
    return core::Error::RemoteProtocol("JSON-RPC stream response must use text/event-stream")
        .WithTransport("jsonrpc")
        .WithHttpStatus(response_metadata_.status_code);
  }

  core::Result<void> EnsureMetadata(const HttpClientResponse& response) {
    if (metadata_validated_) {
      return {};
    }
    return ValidateMetadata(response);
  }

  core::Result<void> HandleChunk(std::string_view chunk) {
    if (state_->cancel_requested.load()) {
      return {};
    }
    if (!metadata_validated_) {
      return core::Error::RemoteProtocol("JSON-RPC stream metadata must be validated before body chunks")
          .WithTransport("jsonrpc");
    }
    if (is_json_response_) {
      json_response_body_.append(chunk);
      return {};
    }
    return parser_.Feed(chunk, [this](const SseEvent& event) { return DispatchEvent(event); });
  }

  core::Result<void> DispatchEvent(const SseEvent& event) {
    if (state_->cancel_requested.load()) {
      return {};
    }
    return DispatchJsonRpcSseEvent(event, request_id_, response_metadata_, observer_);
  }

  bool StopIfCancelled() {
    if (!state_->cancel_requested.load()) {
      return false;
    }
    MarkInactive(*state_);
    return true;
  }

  void HandleJsonResponse(const HttpClientResponse& stream_response) {
    if (json_response_body_.empty()) {
      json_response_body_ = stream_response.body;
    }
    HttpClientResponse json_response = response_metadata_;
    json_response.body = std::move(json_response_body_);
    const auto result = ParseResponseResult(json_response, request_id_);
    if (!result.ok()) {
      NotifyErrorAndStop(*state_, observer_, result.error());
      return;
    }
    if (json_response.status_code < kHttpOkMin || json_response.status_code > kHttpOkMax) {
      NotifyErrorAndStop(*state_, observer_, BuildJsonRpcStreamStatusError(json_response));
      return;
    }
    NotifyErrorAndStop(*state_, observer_,
                       BuildJsonRpcEnvelopeError(kStreamingSuccessRequiresSseMessage, json_response));
  }

  const HttpStreamRequester& stream_requester_;
  const HttpRequest& http_request_;
  StreamObserver& observer_;
  std::shared_ptr<StreamHandle::State> state_;
  std::string request_id_;
  SseParser parser_;
  HttpClientResponse response_metadata_;
  std::string json_response_body_;
  bool metadata_validated_ = false;
  bool is_json_response_ = false;
};

void RunJsonRpcSseWorker(const HttpStreamRequester& stream_requester, const HttpRequest& http_request,
                         StreamObserver& observer, const std::shared_ptr<StreamHandle::State>& state,
                         std::string request_id) {
  JsonRpcSseSession session(stream_requester, http_request, observer, state, std::move(request_id));
  session.Run();
}

}  // namespace

JsonRpcTransport::JsonRpcTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                   std::chrono::milliseconds default_timeout, RequestIdGenerator id_generator)
    : JsonRpcTransport(std::move(resolved_interface), std::move(requester), {}, default_timeout,
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
  return std::make_unique<JsonRpcTransport>(std::move(resolved_interface), MakeDefaultHttpRequester(),
                                            MakeDefaultHttpStreamRequester(), default_timeout, std::move(id_generator));
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
  http_request.headers["Content-Type"] = "application/json";
  http_request.headers["Accept"] = "application/json";
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

  if (response.value().status_code < kHttpOkMin || response.value().status_code > kHttpOkMax) {
    return core::Error::RemoteProtocol("JSON-RPC response received with non-success HTTP status")
        .WithTransport("jsonrpc")
        .WithHttpStatus(response.value().status_code);
  }

  return result.value();
}

core::Result<lf::a2a::v1::SendMessageResponse> JsonRpcTransport::SendMessage(
    const lf::a2a::v1::SendMessageRequest& request, const CallOptions& options) {
  const auto result = InvokeForResultValue(core::json_rpc::MethodNames::kSendMessage, request, options);
  if (!result.ok()) {
    return result.error();
  }

  const auto response = ParseResultMessage<lf::a2a::v1::SendMessageResponse>(result.value(), kHttpOkMin);
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
  const auto response = ParseResultMessage<lf::a2a::v1::Task>(result.value(), kHttpOkMin);
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

  const auto result = InvokeForResultValue(core::json_rpc::MethodNames::kListTasks, params, options);
  if (!result.ok()) {
    return result.error();
  }

  if (!result.value().has_struct_value()) {
    return core::Error::Serialization("ListTasks JSON-RPC result must be an object")
        .WithTransport("jsonrpc")
        .WithHttpStatus(kHttpOkMin);
  }

  ListTasksResponse parsed;
  const auto& fields = result.value().struct_value().fields();
  const auto tasks_it = fields.find("tasks");
  if (tasks_it != fields.end()) {
    if (!tasks_it->second.has_list_value()) {
      return core::Error::Serialization("ListTasks JSON-RPC result field 'tasks' must be an array")
          .WithTransport("jsonrpc")
          .WithHttpStatus(kHttpOkMin);
    }
    for (const auto& task_value : tasks_it->second.list_value().values()) {
      const auto task_json = core::MessageToJson(task_value);
      if (!task_json.ok()) {
        return task_json.error().WithTransport("jsonrpc").WithHttpStatus(kHttpOkMin);
      }
      lf::a2a::v1::Task task;
      const auto task_parse = core::JsonToMessage(task_json.value(), &task, {.ignore_unknown_fields = true});
      if (!task_parse.ok()) {
        return task_parse.error().WithTransport("jsonrpc").WithHttpStatus(kHttpOkMin);
      }
      parsed.tasks.push_back(std::move(task));
    }
  }

  const auto next_page_token_it = fields.find("nextPageToken");
  if (next_page_token_it != fields.end()) {
    if (next_page_token_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
      return core::Error::Serialization("ListTasks JSON-RPC result field 'nextPageToken' must be a string")
          .WithTransport("jsonrpc")
          .WithHttpStatus(kHttpOkMin);
    }
    parsed.next_page_token = next_page_token_it->second.string_value();
  }

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
  const auto response = ParseResultMessage<lf::a2a::v1::Task>(result.value(), kHttpOkMin);
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
  const auto response = ParseResultMessage<lf::a2a::v1::TaskPushNotificationConfig>(result.value(), kHttpOkMin);
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
  const auto response = ParseResultMessage<lf::a2a::v1::TaskPushNotificationConfig>(result.value(), kHttpOkMin);
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
      ParseResultMessage<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(result.value(), kHttpOkMin);
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

  const auto parsed_empty = ParseResultMessage<google::protobuf::Empty>(result.value(), kHttpOkMin);
  if (!parsed_empty.ok()) {
    return parsed_empty.error();
  }
  return {};
}

core::Result<std::unique_ptr<StreamHandle>> JsonRpcTransport::StartSseStream(std::string_view method_name,
                                                                             const google::protobuf::Message& request,
                                                                             StreamObserver& observer,
                                                                             const CallOptions& options) const {
  if (resolved_interface_.transport != PreferredTransport::kJsonRpc) {
    return core::Error::Validation("JsonRpcTransport requires a JSON-RPC interface");
  }
  if (resolved_interface_.url.empty()) {
    return core::Error::Validation("Resolved JSON-RPC interface URL is required");
  }
  if (stream_requester_ == nullptr) {
    return core::Error::Internal("HTTP stream requester is not configured");
  }
  const std::string request_id = id_generator_();
  if (request_id.empty()) {
    return core::Error::Internal("JSON-RPC request id generator returned an empty id");
  }
  const auto envelope_json = BuildJsonRpcEnvelope(method_name, request, request_id);
  if (!envelope_json.ok()) {
    return envelope_json.error();
  }
  HttpRequest http_request;
  http_request.method = std::string(core::http::kMethodPost);
  http_request.url = JoinUrl(resolved_interface_.url);
  http_request.body = envelope_json.value();
  http_request.timeout = options.timeout.value_or(default_timeout_);
  http_request.headers = options.headers;
  http_request.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  http_request.headers["Content-Type"] = "application/json";
  http_request.headers["Accept"] = "text/event-stream";
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
  auto state = std::make_shared<StreamHandle::State>();
  StreamHandle::WorkerThread worker(
      [stream_requester = stream_requester_, http_request = std::move(http_request), &observer, state,
       request_id]() mutable { RunJsonRpcSseWorker(stream_requester, http_request, observer, state, request_id); });
  return std::unique_ptr<StreamHandle>(new StreamHandle(state, std::move(worker)));
}

core::Result<std::unique_ptr<StreamHandle>> JsonRpcTransport::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer, const CallOptions& options) {
  return StartSseStream(core::json_rpc::MethodNames::kSendStreamingMessage, request, observer, options);
}

core::Result<std::unique_ptr<StreamHandle>> JsonRpcTransport::SubscribeTask(const lf::a2a::v1::GetTaskRequest& request,
                                                                            StreamObserver& observer,
                                                                            const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskRequest.id is required");
  }
  return StartSseStream(core::json_rpc::MethodNames::kSubscribeToTask, request, observer, options);
}

}  // namespace a2a::client
