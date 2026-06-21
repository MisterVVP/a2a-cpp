// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/json_rpc_server_transport.h"

#include <google/protobuf/struct.pb.h>

#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/json_rpc.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protocol_error_messages.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/protocol_methods.h"
#include "a2a/core/protojson.h"
#include "a2a/core/task_states.h"
#include "a2a/core/version.h"
#include "a2a/server/http_adapter.h"

namespace a2a::server {
namespace {

template <std::size_t MessageSize>
[[nodiscard]] core::Error InvalidJsonRpcResponsePayload(const std::array<char, MessageSize>& message) {
  return core::protocol_errors::InvalidAgentResponse(core::protocol_error_messages::ToString(message));
}

constexpr int kHttpOk = 200;
constexpr int kHttpInternalServerError = 500;

constexpr int kJsonRpcParseError = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kJsonRpcInvalidParams = -32602;
constexpr int kJsonRpcInternalError = -32603;
constexpr int kJsonRpcVersionNotSupported = -32009;
constexpr std::string_view kSseHeartbeat = ": keep-alive\n\n";
constexpr std::chrono::seconds kSseHeartbeatInterval{15};
constexpr int kJsonRpcServerErrorMin = -32099;
constexpr int kJsonRpcServerErrorMax = -32000;
constexpr std::size_t kMinListTasksPageSize = 1;
constexpr std::string_view kTaskIdJsonField = "taskId";
constexpr std::string_view kPushNotificationConfigJsonField = "pushNotificationConfig";

std::string ToLower(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const auto ch : value) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

std::string FindHeader(const std::unordered_map<std::string, std::string>& headers, std::string_view name) {
  const std::string lowered_name = ToLower(name);
  for (const auto& [header_name, header_value] : headers) {
    if (ToLower(header_name) == lowered_name) {
      return header_value;
    }
  }
  return {};
}

bool HasJsonContentType(const HttpServerRequest& request) {
  const std::string content_type = ToLower(FindHeader(request.headers, "Content-Type"));
  return content_type.empty() || content_type.find("application/json") != std::string::npos;
}

bool IsValidIdType(const google::protobuf::Value& value) {
  return value.kind_case() == ::google::protobuf::Value::kNullValue ||
         value.kind_case() == ::google::protobuf::Value::kStringValue ||
         value.kind_case() == ::google::protobuf::Value::kNumberValue;
}

bool IsMethod(std::string_view actual, std::string_view canonical, std::string_view legacy) {
  return actual == canonical || actual == legacy;
}

bool IsSendMessageMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kSendMessage, core::json_rpc::MethodNames::kSendMessage) ||
         method == core::json_rpc::MethodNames::kLegacySendMessage;
}

bool IsSendStreamingMessageMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kSendStreamingMessage, "a2a.sendStreamingMessage");
}

bool IsGetTaskMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kGetTask, core::json_rpc::MethodNames::kGetTask) ||
         method == core::json_rpc::MethodNames::kLegacyGetTask;
}

bool IsCancelTaskMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kCancelTask, core::json_rpc::MethodNames::kCancelTask) ||
         method == core::json_rpc::MethodNames::kLegacyCancelTask;
}

bool IsListTasksMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kListTasks, core::json_rpc::MethodNames::kListTasks) ||
         method == core::json_rpc::MethodNames::kLegacyListTasks ||
         method == core::json_rpc::MethodNames::kLegacyListTasksDot;
}

bool IsSubscribeToTaskMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kSubscribeToTask, "a2a.subscribeToTask");
}

bool IsCreatePushConfigMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kCreateTaskPushNotificationConfig,
                  core::json_rpc::MethodNames::kCreateTaskPushNotificationConfig);
}

bool IsGetPushConfigMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kGetTaskPushNotificationConfig,
                  core::json_rpc::MethodNames::kGetTaskPushNotificationConfig);
}

bool IsListPushConfigMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kListTaskPushNotificationConfigs,
                  core::json_rpc::MethodNames::kListTaskPushNotificationConfigs);
}

bool IsDeletePushConfigMethod(std::string_view method) {
  return IsMethod(method, core::protocol_methods::kDeleteTaskPushNotificationConfig,
                  core::json_rpc::MethodNames::kDeleteTaskPushNotificationConfig);
}

std::optional<DispatcherOperation> MethodToOperation(std::string_view method) {
  if (IsSendMessageMethod(method)) {
    return DispatcherOperation::kSendMessage;
  }
  if (IsSendStreamingMessageMethod(method)) {
    return DispatcherOperation::kSendStreamingMessage;
  }
  if (IsGetTaskMethod(method)) {
    return DispatcherOperation::kGetTask;
  }
  if (IsSubscribeToTaskMethod(method)) {
    return DispatcherOperation::kSubscribeTask;
  }
  if (IsCancelTaskMethod(method)) {
    return DispatcherOperation::kCancelTask;
  }
  if (IsListTasksMethod(method)) {
    return DispatcherOperation::kListTasks;
  }
  return std::nullopt;
}

core::Result<google::protobuf::Struct> ParseJsonObject(std::string_view body) {
  google::protobuf::Struct envelope;
  const auto parsed = core::JsonToMessage(body, &envelope);
  if (!parsed.ok()) {
    return parsed.error();
  }
  return envelope;
}

core::Result<google::protobuf::Value> FindIdField(const google::protobuf::Struct& envelope) {
  const auto& fields = envelope.fields();
  const auto id_it = fields.find("id");
  if (id_it == fields.end() || !IsValidIdType(id_it->second)) {
    return core::Error::Validation("JSON-RPC request id must be a string, number, or null");
  }
  return id_it->second;
}

core::Result<std::string> FindMethodField(const google::protobuf::Struct& envelope) {
  const auto& fields = envelope.fields();
  const auto method_it = fields.find("method");
  if (method_it == fields.end() || method_it->second.kind_case() != ::google::protobuf::Value::kStringValue ||
      method_it->second.string_value().empty()) {
    return core::Error::Validation("JSON-RPC request method must be a non-empty string");
  }
  return method_it->second.string_value();
}

core::Result<std::string> FindMethodField(std::string_view body) {
  const auto envelope = ParseJsonObject(body);
  if (!envelope.ok()) {
    return envelope.error();
  }
  return FindMethodField(envelope.value());
}

core::Result<google::protobuf::Struct> FindParamsField(const google::protobuf::Struct& envelope) {
  const auto& fields = envelope.fields();
  const auto params_it = fields.find("params");
  if (params_it == fields.end()) {
    return google::protobuf::Struct{};
  }
  if (!params_it->second.has_struct_value()) {
    return core::Error::Validation("JSON-RPC request params must be an object");
  }
  return params_it->second.struct_value();
}

core::Result<void> ValidateJsonRpcVersion(const google::protobuf::Struct& envelope) {
  const auto& fields = envelope.fields();
  const auto version_it = fields.find("jsonrpc");
  if (version_it == fields.end() || version_it->second.kind_case() != ::google::protobuf::Value::kStringValue ||
      version_it->second.string_value() != core::json_rpc::kVersion) {
    return core::Error::Validation("JSON-RPC request has invalid version");
  }
  return {};
}

template <typename T>
core::Result<T> ParseProtoPayload(const google::protobuf::Struct& params) {
  const auto params_json = core::MessageToJson(params);
  if (!params_json.ok()) {
    return params_json.error();
  }
  T payload;
  const auto parse_payload = core::JsonToMessage(params_json.value(), &payload, {.ignore_unknown_fields = true});
  if (!parse_payload.ok()) {
    return parse_payload.error();
  }
  return payload;
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig> ParseCreatePushConfigPayload(
    const google::protobuf::Struct& params) {
  const auto& fields = params.fields();
  const auto nested_config_it = fields.find(std::string(kPushNotificationConfigJsonField));
  if (nested_config_it == fields.end()) {
    return ParseProtoPayload<lf::a2a::v1::TaskPushNotificationConfig>(params);
  }
  if (!nested_config_it->second.has_struct_value()) {
    return core::Error::Validation("TaskPushNotificationConfig.pushNotificationConfig must be an object");
  }

  auto payload = ParseProtoPayload<lf::a2a::v1::TaskPushNotificationConfig>(nested_config_it->second.struct_value());
  if (!payload.ok()) {
    return payload.error();
  }

  const auto task_id_it = fields.find(std::string(kTaskIdJsonField));
  if (task_id_it != fields.end()) {
    if (task_id_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
      return core::Error::Validation("TaskPushNotificationConfig.taskId must be a string");
    }
    payload.value().set_task_id(task_id_it->second.string_value());
  }

  return payload.value();
}

core::Result<void> ParseListTasksPageSize(const google::protobuf::Struct& params,
                                          const JsonRpcServerTransportOptions& options, ListTasksRequest* payload) {
  const auto& fields = params.fields();
  const auto it = fields.find("pageSize");
  if (it == fields.end()) {
    return {};
  }
  if (it->second.kind_case() != ::google::protobuf::Value::kNumberValue) {
    return core::Error::Validation("ListTasksRequest.pageSize must be a number");
  }
  const double page_size = it->second.number_value();
  if (page_size < static_cast<double>(kMinListTasksPageSize) ||
      page_size > static_cast<double>(options.max_list_tasks_page_size)) {
    return core::Error::Validation("ListTasksRequest.pageSize must be between 1 and " +
                                   std::to_string(options.max_list_tasks_page_size));
  }
  payload->page_size = static_cast<std::size_t>(page_size);
  return {};
}

core::Result<void> ParseListTasksContextId(const google::protobuf::Struct& params, ListTasksRequest* payload) {
  const auto& fields = params.fields();
  const auto context_id_it = fields.find("contextId");
  if (context_id_it != fields.end()) {
    if (context_id_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
      return core::Error::Validation("ListTasksRequest.contextId must be a string");
    }
    payload->context_id = context_id_it->second.string_value();
  }

  const auto snake_context_id_it = fields.find("context_id");
  if (!payload->context_id.empty() || snake_context_id_it == fields.end()) {
    return {};
  }
  if (snake_context_id_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
    return core::Error::Validation("ListTasksRequest.context_id must be a string");
  }
  payload->context_id = snake_context_id_it->second.string_value();
  return {};
}

core::Result<void> ParseListTasksPageToken(const google::protobuf::Struct& params, ListTasksRequest* payload) {
  const auto& fields = params.fields();
  const auto page_token_it = fields.find("pageToken");
  if (page_token_it == fields.end()) {
    return {};
  }
  if (page_token_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
    return core::Error::Validation("ListTasksRequest.pageToken must be a string");
  }
  payload->page_token = page_token_it->second.string_value();
  if (payload->page_token.empty()) {
    return {};
  }

  std::uint64_t parsed_offset = 0;
  const auto* begin = payload->page_token.data();
  const auto* end = begin + payload->page_token.size();
  const auto parsed = std::from_chars(begin, end, parsed_offset);
  if (parsed.ec != std::errc() || parsed.ptr != end) {
    return core::Error::Validation("ListTasksRequest.pageToken must be a valid offset");
  }
  return {};
}

core::Result<void> ParseListTasksStatus(const google::protobuf::Struct& params, ListTasksRequest* payload) {
  const auto& fields = params.fields();
  const auto status_it = fields.find("status");
  if (status_it == fields.end()) {
    return {};
  }
  if (status_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
    return core::Error::Validation("ListTasksRequest.status must be a string");
  }
  lf::a2a::v1::TaskState state = lf::a2a::v1::TASK_STATE_UNSPECIFIED;
  if (!lf::a2a::v1::TaskState_Parse(status_it->second.string_value(), &state)) {
    return core::Error::Validation("ListTasksRequest.status must be a valid TaskState value");
  }
  payload->status_filter = state;
  return {};
}

core::Result<void> ParseListTasksTimestampAfter(const google::protobuf::Struct& params, ListTasksRequest* payload) {
  const auto& fields = params.fields();
  const auto timestamp_after_it = fields.find("statusTimestampAfter");
  if (timestamp_after_it == fields.end()) {
    return {};
  }
  if (timestamp_after_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
    return core::Error::Validation("ListTasksRequest.statusTimestampAfter must be a string");
  }

  google::protobuf::Timestamp ts;
  const auto parsed_ts = core::JsonToMessage("\"" + timestamp_after_it->second.string_value() + "\"", &ts);
  if (!parsed_ts.ok()) {
    return core::Error::Validation("ListTasksRequest.statusTimestampAfter must be an RFC3339 timestamp");
  }
  payload->status_timestamp_after = ts;
  return {};
}

core::Result<void> ParseListTasksHistoryLength(const google::protobuf::Struct& params, ListTasksRequest* payload) {
  const auto& fields = params.fields();
  const auto history_length_it = fields.find("historyLength");
  if (history_length_it == fields.end()) {
    return {};
  }
  if (history_length_it->second.kind_case() != ::google::protobuf::Value::kNumberValue ||
      history_length_it->second.number_value() < 0) {
    return core::Error::Validation("ListTasksRequest.historyLength must be non-negative");
  }
  payload->history_length = static_cast<std::size_t>(history_length_it->second.number_value());
  return {};
}

core::Result<void> ParseListTasksIncludeArtifacts(const google::protobuf::Struct& params, ListTasksRequest* payload) {
  const auto& fields = params.fields();
  const auto include_artifacts_it = fields.find("includeArtifacts");
  if (include_artifacts_it == fields.end()) {
    return {};
  }
  if (include_artifacts_it->second.kind_case() != ::google::protobuf::Value::kBoolValue) {
    return core::Error::Validation("ListTasksRequest.includeArtifacts must be a boolean");
  }
  payload->include_artifacts = include_artifacts_it->second.bool_value();
  return {};
}

core::Result<void> ApplyListTasksParsers(const google::protobuf::Struct& params,
                                         const JsonRpcServerTransportOptions& options, ListTasksRequest* payload) {
  const auto page_size = ParseListTasksPageSize(params, options, payload);
  if (!page_size.ok()) {
    return page_size.error();
  }
  const auto context_id = ParseListTasksContextId(params, payload);
  if (!context_id.ok()) {
    return context_id.error();
  }
  const auto page_token = ParseListTasksPageToken(params, payload);
  if (!page_token.ok()) {
    return page_token.error();
  }
  const auto status = ParseListTasksStatus(params, payload);
  if (!status.ok()) {
    return status.error();
  }
  const auto timestamp_after = ParseListTasksTimestampAfter(params, payload);
  if (!timestamp_after.ok()) {
    return timestamp_after.error();
  }
  const auto history_length = ParseListTasksHistoryLength(params, payload);
  if (!history_length.ok()) {
    return history_length.error();
  }
  return ParseListTasksIncludeArtifacts(params, payload);
}

core::Result<ListTasksRequest> ParseListTasksPayload(const google::protobuf::Struct& params,
                                                     const JsonRpcServerTransportOptions& options) {
  if (options.max_list_tasks_page_size < kMinListTasksPageSize) {
    return core::Error::Internal("JSON-RPC max_list_tasks_page_size must be at least 1");
  }

  ListTasksRequest payload;
  const auto parsed = ApplyListTasksParsers(params, options, &payload);
  if (!parsed.ok()) {
    return parsed.error();
  }
  if (payload.page_size == 0) {
    payload.page_size = options.default_list_tasks_page_size;
  }
  return payload;
}

core::Result<DispatchRequest> BuildDispatchRequestFromMethod(std::string_view method_name,
                                                             const google::protobuf::Struct& params,
                                                             const JsonRpcServerTransportOptions& options) {
  if (IsCreatePushConfigMethod(method_name)) {
    auto payload = ParseCreatePushConfigPayload(params);
    if (!payload.ok()) {
      return payload.error();
    }
    return DispatchRequest{.operation = DispatcherOperation::kCreateTaskPushNotificationConfig,
                           .payload = std::move(payload.value())};
  }
  if (IsGetPushConfigMethod(method_name)) {
    auto payload = ParseProtoPayload<lf::a2a::v1::GetTaskPushNotificationConfigRequest>(params);
    if (!payload.ok()) {
      return payload.error();
    }
    return DispatchRequest{.operation = DispatcherOperation::kGetTaskPushNotificationConfig,
                           .payload = std::move(payload.value())};
  }
  if (IsListPushConfigMethod(method_name)) {
    auto payload = ParseProtoPayload<lf::a2a::v1::ListTaskPushNotificationConfigsRequest>(params);
    if (!payload.ok()) {
      return payload.error();
    }
    return DispatchRequest{.operation = DispatcherOperation::kListTaskPushNotificationConfigs,
                           .payload = std::move(payload.value())};
  }
  if (IsDeletePushConfigMethod(method_name)) {
    auto payload = ParseProtoPayload<lf::a2a::v1::DeleteTaskPushNotificationConfigRequest>(params);
    if (!payload.ok()) {
      return payload.error();
    }
    return DispatchRequest{.operation = DispatcherOperation::kDeleteTaskPushNotificationConfig,
                           .payload = std::move(payload.value())};
  }

  const auto operation = MethodToOperation(method_name);
  if (!operation.has_value()) {
    return core::Error::RemoteProtocol("JSON-RPC method is not supported")
        .WithProtocolCode(std::to_string(kJsonRpcMethodNotFound));
  }

  DispatchRequest dispatch_request;
  dispatch_request.operation = operation.value();

  switch (dispatch_request.operation) {
    case DispatcherOperation::kSendMessage:
    case DispatcherOperation::kSendStreamingMessage: {
      auto payload = ParseProtoPayload<lf::a2a::v1::SendMessageRequest>(params);
      if (!payload.ok()) {
        return payload.error();
      }
      dispatch_request.payload = std::move(payload.value());
      return dispatch_request;
    }
    case DispatcherOperation::kGetTask:
    case DispatcherOperation::kSubscribeTask: {
      auto payload = ParseProtoPayload<lf::a2a::v1::GetTaskRequest>(params);
      if (!payload.ok()) {
        return payload.error();
      }
      dispatch_request.payload = std::move(payload.value());
      return dispatch_request;
    }
    case DispatcherOperation::kCancelTask: {
      auto payload = ParseProtoPayload<lf::a2a::v1::CancelTaskRequest>(params);
      if (!payload.ok()) {
        return payload.error();
      }
      dispatch_request.payload = std::move(payload.value());
      return dispatch_request;
    }
    case DispatcherOperation::kListTasks: {
      auto payload = ParseListTasksPayload(params, options);
      if (!payload.ok()) {
        return payload.error();
      }
      const auto& parsed_payload = payload.value();
      dispatch_request.payload = parsed_payload;
      return dispatch_request;
    }
    case DispatcherOperation::kCreateTaskPushNotificationConfig:
    case DispatcherOperation::kGetTaskPushNotificationConfig:
    case DispatcherOperation::kListTaskPushNotificationConfigs:
    case DispatcherOperation::kDeleteTaskPushNotificationConfig:
      return core::Error::Internal("Push notification operations are handled before the generic JSON-RPC switch");
  }

  return core::Error::Internal("Unsupported JSON-RPC dispatcher operation");
}

core::Result<google::protobuf::Value> BuildJsonValueFromMessage(const google::protobuf::Message& message) {
  const auto json = core::MessageToJson(message);
  if (!json.ok()) {
    return json.error();
  }

  google::protobuf::Value value;
  const auto parsed = core::JsonToMessage(json.value(), &value);
  if (!parsed.ok()) {
    return parsed.error();
  }
  return value;
}

core::Result<google::protobuf::Value> BuildListTasksResult(const ListTasksResponse& list_response) {
  google::protobuf::Struct result;
  auto* fields = result.mutable_fields();

  google::protobuf::Value tasks_value;
  auto* list = tasks_value.mutable_list_value();
  for (const auto& task : list_response.tasks) {
    const auto task_json_value = BuildJsonValueFromMessage(task);
    if (!task_json_value.ok()) {
      return task_json_value.error();
    }
    *list->add_values() = task_json_value.value();
  }
  (*fields)["tasks"] = std::move(tasks_value);

  google::protobuf::Value page_size_value;
  page_size_value.set_number_value(static_cast<double>(list_response.page_size));
  (*fields)["pageSize"] = std::move(page_size_value);

  google::protobuf::Value total_size_value;
  total_size_value.set_number_value(static_cast<double>(list_response.total_size));
  (*fields)["totalSize"] = std::move(total_size_value);

  google::protobuf::Value token;
  token.set_string_value(list_response.next_page_token);
  (*fields)["nextPageToken"] = std::move(token);

  google::protobuf::Value wrapper;
  *wrapper.mutable_struct_value() = std::move(result);
  return wrapper;
}

int HttpStatusFromError(const core::Error& error) {
  (void)error;
  return kHttpOk;
}

std::string ErrorInfoReason(const core::Error& error) {
  const auto& protocol_code = error.protocol_code();
  if (protocol_code.has_value()) {
    if (*protocol_code == core::protocol_codes::kTaskNotFound) {
      return "TASK_NOT_FOUND";
    }
    if (*protocol_code == core::protocol_codes::kTaskNotCancelable) {
      return "TASK_NOT_CANCELABLE";
    }
    if (*protocol_code == core::protocol_codes::kPushNotificationNotSupported) {
      return "PUSH_NOTIFICATION_NOT_SUPPORTED";
    }
    if (*protocol_code == core::protocol_codes::kUnsupportedOperation) {
      return "UNSUPPORTED_OPERATION";
    }
    if (*protocol_code == core::protocol_codes::kContentTypeNotSupported) {
      return "CONTENT_TYPE_NOT_SUPPORTED";
    }
    if (*protocol_code == core::protocol_codes::kInvalidAgentResponse) {
      return "INVALID_AGENT_RESPONSE";
    }
    if (*protocol_code == core::protocol_codes::kVersionNotSupported) {
      return "VERSION_NOT_SUPPORTED";
    }
  }

  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return "INVALID_PARAMS";
    case core::ErrorCode::kUnsupportedVersion:
      return "VERSION_NOT_SUPPORTED";
    case core::ErrorCode::kRemoteProtocol:
      return "UNSUPPORTED_OPERATION";
    case core::ErrorCode::kNetwork:
    case core::ErrorCode::kSerialization:
    case core::ErrorCode::kInternal:
      return "INVALID_AGENT_RESPONSE";
  }
  return "INVALID_AGENT_RESPONSE";
}

int JsonRpcCodeFromError(const core::Error& error) {
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return kJsonRpcInvalidParams;
    case core::ErrorCode::kUnsupportedVersion:
      return kJsonRpcVersionNotSupported;
    case core::ErrorCode::kRemoteProtocol: {
      const auto& protocol_code = error.protocol_code();
      if (protocol_code.has_value()) {
        int parsed_code = 0;
        const auto* begin = protocol_code->data();
        const auto* end = begin + protocol_code->size();
        const auto parse = std::from_chars(begin, end, parsed_code);
        if (parse.ec == std::errc() && parse.ptr == end && parsed_code <= kJsonRpcServerErrorMax) {
          return parsed_code;
        }
      }
      return kJsonRpcInternalError;
    }
    case core::ErrorCode::kNetwork:
    case core::ErrorCode::kSerialization:
    case core::ErrorCode::kInternal:
      return kJsonRpcInternalError;
  }
  return kJsonRpcInternalError;
}

core::Result<void> AppendSseJsonRpcEvent(std::string& body, const google::protobuf::Value& id,
                                         const lf::a2a::v1::StreamResponse& event) {
  const auto event_value = BuildJsonValueFromMessage(event);
  if (!event_value.ok()) {
    return event_value.error();
  }

  google::protobuf::Struct envelope;
  auto* fields = envelope.mutable_fields();
  (*fields)["jsonrpc"].set_string_value(std::string(core::json_rpc::kVersion));
  (*fields)["id"] = id;
  (*fields)["result"] = event_value.value();

  const auto serialized = core::MessageToJson(envelope);
  if (!serialized.ok()) {
    return serialized.error();
  }
  body += "data: ";
  body += serialized.value();
  body += "\n\n";
  return {};
}

core::Result<void> WriteSseChunk(HttpByteTransport& transport, std::string_view chunk) {
  std::size_t sent = 0;
  while (sent < chunk.size()) {
    const auto written = transport.Write(chunk.data() + sent, chunk.size() - sent);
    if (!written.ok()) {
      return written.error();
    }
    if (written.value() == 0) {
      return core::Error::Internal("Transport write returned zero bytes while streaming JSON-RPC SSE");
    }
    sent += written.value();
  }
  return {};
}

core::Result<void> StreamJsonRpcSseEvents(const google::protobuf::Value& id,
                                          const std::shared_ptr<std::unique_ptr<ServerStreamSession>>& session,
                                          HttpByteTransport& transport) {
  if (*session == nullptr) {
    return core::Error::Internal("JSON-RPC streaming session is missing");
  }

  auto next = (*session)->NextFor(kSseHeartbeatInterval);
  for (; next.ok(); next = (*session)->NextFor(kSseHeartbeatInterval)) {
    const auto& event = next.value();
    if (!event.has_value()) {
      if (!(*session)->IsLive()) {
        return {};
      }
      const auto heartbeat = WriteSseChunk(transport, kSseHeartbeat);
      if (!heartbeat.ok()) {
        (*session)->Cancel();
        return heartbeat.error();
      }
      continue;
    }
    std::string chunk;
    const auto append = AppendSseJsonRpcEvent(chunk, id, event.value());
    if (!append.ok()) {
      return append.error();
    }
    const auto written = WriteSseChunk(transport, chunk);
    if (!written.ok()) {
      (*session)->Cancel();
      return written.error();
    }
  }
  return next.error();
}

core::Result<void> BufferJsonRpcSseEvents(const google::protobuf::Value& id, ServerStreamSession& session,
                                          std::string& body) {
  auto next = session.Next();
  for (; next.ok(); next = session.Next()) {
    const auto& event = next.value();
    if (!event.has_value()) {
      return {};
    }
    const auto append = AppendSseJsonRpcEvent(body, id, event.value());
    if (!append.ok()) {
      return append.error();
    }
  }
  return next.error();
}

core::Result<HttpServerResponse> BuildSseResponse(const google::protobuf::Value& id,
                                                  std::unique_ptr<ServerStreamSession>& session) {
  if (session == nullptr) {
    return core::Error::Internal("JSON-RPC streaming session is missing");
  }

  HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers["Content-Type"] = "text/event-stream";
  response.headers["Cache-Control"] = "no-cache";
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();

  if (session->IsLive()) {
    auto session_holder = std::make_shared<std::unique_ptr<ServerStreamSession>>(std::move(session));
    response.stream_writer = [id, session_holder](HttpByteTransport& transport) -> core::Result<void> {
      return StreamJsonRpcSseEvents(id, session_holder, transport);
    };
    return response;
  }

  const auto buffered = BufferJsonRpcSseEvents(id, *session, response.body);
  if (!buffered.ok()) {
    return buffered.error();
  }
  return response;
}

}  // namespace

JsonRpcServerTransport::JsonRpcServerTransport(Dispatcher* dispatcher, JsonRpcServerTransportOptions options)
    : dispatcher_(dispatcher), options_(std::move(options)) {
  options_.rpc_path = NormalizePath(std::move(options_.rpc_path));
}

core::Result<HttpServerResponse> JsonRpcServerTransport::Handle(const HttpServerRequest& request) const {
  if (dispatcher_ == nullptr) {
    return core::Error::Internal("JSON-RPC server dispatcher is not configured");
  }

  const std::string normalized_target = NormalizePath(request.target);
  if (request.method != "POST" || normalized_target != options_.rpc_path) {
    return BuildErrorResponse(kJsonRpcInvalidRequest, "No matching JSON-RPC route", ResponseId{}, std::nullopt,
                              kHttpOk);
  }

  if (!HasJsonContentType(request)) {
    const auto error = core::protocol_errors::ContentTypeNotSupported().WithTransport("jsonrpc");
    return BuildErrorResponse(JsonRpcCodeFromError(error), error.message(), ResponseId{}, error, kHttpOk);
  }

  const auto version = ValidateVersionHeader(request);
  if (!version.ok()) {
    const auto error = version.error().WithTransport("jsonrpc");
    return BuildErrorResponse(JsonRpcCodeFromError(error), error.message(), ResponseId{}, error, kHttpOk);
  }

  const auto parsed = ParseRequest(request.body, options_);
  if (!parsed.ok()) {
    int parse_code = JsonRpcCodeFromError(parsed.error());
    switch (parsed.error().code()) {
      case core::ErrorCode::kValidation:
        parse_code = kJsonRpcInvalidParams;
        break;
      case core::ErrorCode::kSerialization:
        parse_code = kJsonRpcParseError;
        break;
      case core::ErrorCode::kUnsupportedVersion:
      case core::ErrorCode::kNetwork:
      case core::ErrorCode::kRemoteProtocol:
      case core::ErrorCode::kInternal:
        break;
    }
    return BuildErrorResponse(parse_code, parsed.error().message(), ResponseId{}, parsed.error(), kHttpOk);
  }

  const auto method = FindMethodField(request.body);
  const bool is_streaming = method.ok() && IsSendStreamingMessageMethod(method.value());
  const bool is_subscribe = method.ok() && IsSubscribeToTaskMethod(method.value());

  RequestContext context;
  context.remote_address = request.remote_address.empty() ? std::optional<std::string>{}
                                                          : std::optional<std::string>(request.remote_address);
  context.client_headers = request.headers;
  context.auth_metadata = ExtractAuthMetadata(request.headers);

  auto dispatch = dispatcher_->Dispatch(parsed.value().dispatch, context);
  if (!dispatch.ok()) {
    const int http_status = HttpStatusFromError(dispatch.error());
    return BuildErrorResponse(JsonRpcCodeFromError(dispatch.error()), dispatch.error().message(), parsed.value().id,
                              dispatch.error().WithTransport("jsonrpc"), http_status);
  }

  if (is_streaming) {
    auto* session = std::get_if<std::unique_ptr<ServerStreamSession>>(&dispatch.value().payload());
    if (session == nullptr) {
      const auto error =
          InvalidJsonRpcResponsePayload(core::protocol_error_messages::kJsonRpcResponsePayloadMismatchForStreaming)
              .WithTransport("jsonrpc");
      return BuildErrorResponse(JsonRpcCodeFromError(error), error.message(), parsed.value().id, error,
                                HttpStatusFromError(error));
    }
    const auto sse = BuildSseResponse(parsed.value().id.value(), *session);
    if (!sse.ok()) {
      const auto error = sse.error().WithTransport("jsonrpc");
      return BuildErrorResponse(JsonRpcCodeFromError(error), error.message(), parsed.value().id, error,
                                HttpStatusFromError(error));
    }
    return sse.value();
  }

  if (is_subscribe) {
    auto* session = std::get_if<std::unique_ptr<ServerStreamSession>>(&dispatch.value().payload());
    if (session == nullptr || *session == nullptr) {
      const auto error =
          InvalidJsonRpcResponsePayload(core::protocol_error_messages::kJsonRpcResponsePayloadMismatchForSubscribe)
              .WithTransport("jsonrpc");
      return BuildErrorResponse(JsonRpcCodeFromError(error), error.message(), parsed.value().id, error,
                                HttpStatusFromError(error));
    }
    const auto sse = BuildSseResponse(parsed.value().id.value(), *session);
    if (!sse.ok()) {
      const auto error = sse.error().WithTransport("jsonrpc");
      return BuildErrorResponse(JsonRpcCodeFromError(error), error.message(), parsed.value().id, error,
                                HttpStatusFromError(error));
    }
    return sse.value();
  }

  const auto result = SerializeDispatchResult(parsed.value().dispatch, dispatch.value());
  if (!result.ok()) {
    const auto tagged = result.error().WithTransport("jsonrpc");
    return BuildErrorResponse(JsonRpcCodeFromError(tagged), tagged.message(), parsed.value().id, tagged,
                              HttpStatusFromError(tagged));
  }

  return BuildSuccessResponse(parsed.value().id, result.value());
}

core::Result<void> JsonRpcServerTransport::ValidateVersionHeader(const HttpServerRequest& request) const {
  const std::string version = FindHeader(request.headers, core::Version::kHeaderName);
  if (version.empty()) {
    if (options_.require_version_header) {
      return core::protocol_errors::VersionNotSupported("Missing required A2A-Version header");
    }
    return {};
  }

  if (!core::Version::IsSupported(version)) {
    return core::protocol_errors::VersionNotSupported("Unsupported A2A-Version header value");
  }

  return {};
}

core::Result<JsonRpcServerTransport::JsonRpcRequest> JsonRpcServerTransport::ParseRequest(
    std::string_view body, const JsonRpcServerTransportOptions& options) {
  const auto envelope = ParseJsonObject(body);
  if (!envelope.ok()) {
    return envelope.error();
  }

  const auto version = ValidateJsonRpcVersion(envelope.value());
  if (!version.ok()) {
    return version.error();
  }

  const auto id = FindIdField(envelope.value());
  if (!id.ok()) {
    return id.error();
  }

  const auto method = FindMethodField(envelope.value());
  if (!method.ok()) {
    return method.error();
  }

  const auto params = FindParamsField(envelope.value());
  if (!params.ok()) {
    return params.error();
  }

  const auto dispatch = BuildDispatchRequestFromMethod(method.value(), params.value(), options);
  if (!dispatch.ok()) {
    return dispatch.error();
  }

  return JsonRpcRequest{.id = ResponseId(id.value()), .dispatch = dispatch.value()};
}

core::Result<google::protobuf::Value> JsonRpcServerTransport::SerializeDispatchResult(
    const DispatchRequest& request, const DispatchResponse& response) {
  switch (request.operation) {
    case DispatcherOperation::kSendMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageResponse>(&response.payload());
      if (payload == nullptr) {
        return InvalidJsonRpcResponsePayload(
            core::protocol_error_messages::kJsonRpcResponsePayloadMismatchForSendMessage);
      }
      return BuildJsonValueFromMessage(*payload);
    }
    case DispatcherOperation::kGetTask:
    case DispatcherOperation::kCancelTask: {
      const auto* payload = std::get_if<lf::a2a::v1::Task>(&response.payload());
      if (payload == nullptr) {
        return InvalidJsonRpcResponsePayload(core::protocol_error_messages::kJsonRpcResponsePayloadMismatchForTask);
      }
      return BuildJsonValueFromMessage(*payload);
    }
    case DispatcherOperation::kListTasks: {
      const auto* payload = std::get_if<ListTasksResponse>(&response.payload());
      if (payload == nullptr) {
        return InvalidJsonRpcResponsePayload(
            core::protocol_error_messages::kJsonRpcResponsePayloadMismatchForListTasks);
      }
      return BuildListTasksResult(*payload);
    }
    case DispatcherOperation::kCreateTaskPushNotificationConfig:
    case DispatcherOperation::kGetTaskPushNotificationConfig: {
      const auto* payload = std::get_if<lf::a2a::v1::TaskPushNotificationConfig>(&response.payload());
      if (payload == nullptr) {
        return InvalidJsonRpcResponsePayload(
            core::protocol_error_messages::kJsonRpcResponsePayloadMismatchForPushConfig);
      }
      return BuildJsonValueFromMessage(*payload);
    }
    case DispatcherOperation::kListTaskPushNotificationConfigs: {
      const auto* payload = std::get_if<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(&response.payload());
      if (payload == nullptr) {
        return InvalidJsonRpcResponsePayload(
            core::protocol_error_messages::kJsonRpcResponsePayloadMismatchForPushConfigList);
      }
      return BuildJsonValueFromMessage(*payload);
    }
    case DispatcherOperation::kDeleteTaskPushNotificationConfig: {
      google::protobuf::Value value;
      value.mutable_struct_value();
      return value;
    }
    case DispatcherOperation::kSendStreamingMessage:
    case DispatcherOperation::kSubscribeTask:
      return core::protocol_errors::InvalidAgentResponse("Streaming JSON-RPC responses must be serialized as SSE");
  }

  return core::protocol_errors::InvalidAgentResponse("Unsupported JSON-RPC dispatcher operation");
}

HttpServerResponse JsonRpcServerTransport::BuildSuccessResponse(const ResponseId& id,
                                                                const google::protobuf::Value& result) {
  google::protobuf::Struct envelope;
  auto* fields = envelope.mutable_fields();

  (*fields)["jsonrpc"].set_string_value(std::string(core::json_rpc::kVersion));
  (*fields)["id"] = id.value();
  (*fields)["result"] = result;

  HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers["Content-Type"] = "application/json";
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();

  const auto body = core::MessageToJson(envelope);
  if (body.ok()) {
    response.body = body.value();
  } else {
    response.body = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"Failed to serialize response"}})";
    response.status_code = kHttpInternalServerError;
  }

  return response;
}

HttpServerResponse JsonRpcServerTransport::BuildErrorResponse(int json_rpc_code, std::string_view message,
                                                              const ResponseId& id,
                                                              const std::optional<core::Error>& error,
                                                              int http_status) {
  google::protobuf::Struct envelope;
  auto* fields = envelope.mutable_fields();
  (*fields)["jsonrpc"].set_string_value(std::string(core::json_rpc::kVersion));
  (*fields)["id"] = id.value();

  google::protobuf::Value error_value;
  auto* error_fields = error_value.mutable_struct_value()->mutable_fields();
  (*error_fields)["code"].set_number_value(json_rpc_code);
  (*error_fields)["message"].set_string_value(std::string(message));

  if (error.has_value() && json_rpc_code >= kJsonRpcServerErrorMin && json_rpc_code <= kJsonRpcServerErrorMax) {
    google::protobuf::Value data;
    auto* data_values = data.mutable_list_value()->mutable_values();
    google::protobuf::Value error_info;
    auto* info_fields = error_info.mutable_struct_value()->mutable_fields();
    (*info_fields)["@type"].set_string_value("type.googleapis.com/google.rpc.ErrorInfo");
    (*info_fields)["reason"].set_string_value(ErrorInfoReason(*error));
    (*info_fields)["domain"].set_string_value("a2a-protocol.org");

    const auto& protocol_code = error->protocol_code();
    const auto& transport = error->transport();
    if (protocol_code.has_value() || transport.has_value()) {
      google::protobuf::Value metadata;
      auto* metadata_fields = metadata.mutable_struct_value()->mutable_fields();
      if (protocol_code.has_value()) {
        (*metadata_fields)["protocolCode"].set_string_value(*protocol_code);
      }
      if (transport.has_value()) {
        (*metadata_fields)["transport"].set_string_value(*transport);
      }
      (*info_fields)["metadata"] = std::move(metadata);
    }
    data_values->Add(std::move(error_info));
    (*error_fields)["data"] = std::move(data);
  }

  (*fields)["error"] = std::move(error_value);

  HttpServerResponse response;
  response.status_code = http_status;
  response.headers["Content-Type"] = "application/json";
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();

  const auto body = core::MessageToJson(envelope);
  if (body.ok()) {
    response.body = body.value();
  } else {
    response.body = R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"Failed to serialize error"}})";
    response.status_code = kHttpInternalServerError;
  }

  return response;
}

std::string JsonRpcServerTransport::NormalizePath(std::string path) {
  if (path.empty()) {
    path = "/";
  }
  if (path.front() != '/') {
    path.insert(path.begin(), '/');
  }
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

}  // namespace a2a::server
