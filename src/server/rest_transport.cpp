// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/rest_transport.h"

#include <google/protobuf/struct.pb.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protocol_error_messages.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/protocol_methods.h"
#include "a2a/core/protojson.h"
#include "a2a/core/task_states.h"
#include "a2a/server/http_adapter.h"

namespace a2a::server {
namespace {

template <std::size_t MessageSize>
[[nodiscard]] core::Error InternalResponsePayloadMismatch(const std::array<char, MessageSize>& message) {
  return core::Error::Internal(core::protocol_error_messages::ToString(message));
}

constexpr int kHttpOk = 200;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpConflict = 409;
constexpr int kHttpNotFound = 404;
constexpr int kHttpBadGateway = 502;
constexpr int kHttpServiceUnavailable = 503;
constexpr int kHttpInternalServerError = 500;
constexpr std::size_t kDecimalBase = 10;
constexpr std::string_view kTaskSubscribeSuffix = ":subscribe";

const std::array<RestRoute, 10> kRoutes = {
    RestRoute{.method = "POST",
              .path_pattern = RestEndpointPaths::kSendMessage,
              .operation = DispatcherOperation::kSendMessage},
    RestRoute{.method = "POST",
              .path_pattern = RestEndpointPaths::kSendStreamingMessage,
              .operation = DispatcherOperation::kSendStreamingMessage},
    RestRoute{.method = "GET", .path_pattern = "/tasks/{id}", .operation = DispatcherOperation::kGetTask},
    RestRoute{.method = "GET",
              .path_pattern = RestEndpointPaths::kTaskCollection,
              .operation = DispatcherOperation::kListTasks},
    RestRoute{.method = "POST", .path_pattern = "/tasks/{id}:cancel", .operation = DispatcherOperation::kCancelTask},
    RestRoute{
        .method = "GET", .path_pattern = "/tasks/{id}:subscribe", .operation = DispatcherOperation::kSubscribeTask},
    RestRoute{.method = "POST",
              .path_pattern = "/tasks/{task_id}/pushNotificationConfigs",
              .operation = DispatcherOperation::kCreateTaskPushNotificationConfig},
    RestRoute{.method = "GET",
              .path_pattern = "/tasks/{task_id}/pushNotificationConfigs/{id}",
              .operation = DispatcherOperation::kGetTaskPushNotificationConfig},
    RestRoute{.method = "GET",
              .path_pattern = "/tasks/{task_id}/pushNotificationConfigs",
              .operation = DispatcherOperation::kListTaskPushNotificationConfigs},
    RestRoute{.method = "DELETE",
              .path_pattern = "/tasks/{task_id}/pushNotificationConfigs/{id}",
              .operation = DispatcherOperation::kDeleteTaskPushNotificationConfig},
};

std::optional<std::string> ParseTaskIdFromPath(std::string_view path, bool for_cancel) {
  if (!path.starts_with(RestEndpointPaths::kTaskResourcePrefix)) {
    return std::nullopt;
  }

  std::string suffix(path.substr(RestEndpointPaths::kTaskResourcePrefix.size()));
  if (suffix.empty()) {
    return std::nullopt;
  }

  if (for_cancel) {
    if (!suffix.ends_with(RestEndpointPaths::kTaskCancelSuffix)) {
      return std::nullopt;
    }
    suffix = suffix.substr(0, suffix.size() - RestEndpointPaths::kTaskCancelSuffix.size());
  } else if (suffix.ends_with(RestEndpointPaths::kTaskCancelSuffix) || suffix.ends_with(kTaskSubscribeSuffix)) {
    return std::nullopt;
  }

  if (suffix.empty() || suffix.find('/') != std::string::npos) {
    return std::nullopt;
  }

  return suffix;
}

std::optional<std::string> ParseTaskIdFromActionPath(std::string_view path, std::string_view action_suffix) {
  if (!path.starts_with(RestEndpointPaths::kTaskResourcePrefix) || !path.ends_with(action_suffix)) {
    return std::nullopt;
  }

  const auto id_start = RestEndpointPaths::kTaskResourcePrefix.size();
  const auto id_length = path.size() - id_start - action_suffix.size();
  std::string task_id(path.substr(id_start, id_length));
  if (task_id.empty() || task_id.find('/') != std::string::npos) {
    return std::nullopt;
  }
  return task_id;
}

struct PushConfigPathParts final {
  std::string task_id;
  std::string config_id;
  bool collection = false;
};

std::optional<PushConfigPathParts> ParsePushConfigPath(std::string_view path) {
  if (!path.starts_with(RestEndpointPaths::kTaskResourcePrefix)) {
    return std::nullopt;
  }
  const auto suffix = path.substr(RestEndpointPaths::kTaskResourcePrefix.size());
  const auto segment_pos = suffix.find(core::protocol_methods::kPushNotificationConfigsSegment);
  if (segment_pos == std::string_view::npos || segment_pos == 0) {
    return std::nullopt;
  }
  PushConfigPathParts parts;
  parts.task_id = std::string(suffix.substr(0, segment_pos));
  const auto remainder = suffix.substr(segment_pos + core::protocol_methods::kPushNotificationConfigsSegment.size());
  if (parts.task_id.find('/') != std::string::npos) {
    return std::nullopt;
  }
  if (remainder.empty()) {
    parts.collection = true;
    return parts;
  }
  if (!remainder.starts_with('/') || remainder.size() == 1U) {
    return std::nullopt;
  }
  parts.config_id = std::string(remainder.substr(1));
  if (parts.config_id.find('/') != std::string::npos) {
    return std::nullopt;
  }
  return parts;
}

int ParsePageSize(std::string_view raw_page_size) {
  if (raw_page_size.empty()) {
    return 0;
  }
  std::size_t parsed = 0;
  for (char c : raw_page_size) {
    if (c < '0' || c > '9') {
      return -1;
    }
    parsed = (parsed * kDecimalBase) + static_cast<std::size_t>(c - '0');
  }
  if (parsed > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return -1;
  }
  return static_cast<int>(parsed);
}

std::optional<std::string> LookupQuery(const RestRequest& request, std::string_view key) {
  const auto it = request.query_params.find(std::string(key));
  if (it == request.query_params.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::string ErrorStatusName(int http_status) {
  switch (http_status) {
    case kHttpBadRequest:
      return "INVALID_ARGUMENT";
    case kHttpNotFound:
      return "NOT_FOUND";
    case kHttpConflict:
      return "FAILED_PRECONDITION";
    case kHttpBadGateway:
    case kHttpInternalServerError:
      return "INTERNAL";
    case kHttpServiceUnavailable:
      return "UNAVAILABLE";
    default:
      return "UNKNOWN";
  }
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
  }

  if (error.code() == core::ErrorCode::kUnsupportedVersion) {
    return "VERSION_NOT_SUPPORTED";
  }
  if (error.code() == core::ErrorCode::kRemoteProtocol || error.code() == core::ErrorCode::kValidation) {
    return "UNSUPPORTED_OPERATION";
  }
  return "INVALID_AGENT_RESPONSE";
}

int ToHttpStatus(const core::Error& error) {
  const auto& http_status = error.http_status();
  if (http_status.has_value()) {
    return *http_status;
  }
  if (error.code() == core::ErrorCode::kValidation || error.code() == core::ErrorCode::kUnsupportedVersion) {
    return kHttpBadRequest;
  }
  if (error.code() == core::ErrorCode::kNetwork) {
    return kHttpServiceUnavailable;
  }
  if (error.code() == core::ErrorCode::kRemoteProtocol) {
    return kHttpBadGateway;
  }
  return kHttpInternalServerError;
}

core::Result<std::string> BuildListTasksJson(const ListTasksResponse& response) {
  google::protobuf::Struct payload;
  auto* payload_fields = payload.mutable_fields();

  google::protobuf::Value tasks_value;
  auto* list_value = tasks_value.mutable_list_value();
  for (const auto& task : response.tasks) {
    const auto task_json = core::MessageToJson(task);
    if (!task_json.ok()) {
      return task_json.error();
    }

    google::protobuf::Struct task_struct;
    const auto parsed_task_json = core::JsonToMessage(task_json.value(), &task_struct);
    if (!parsed_task_json.ok()) {
      return parsed_task_json.error();
    }

    google::protobuf::Value task_value;
    *task_value.mutable_struct_value() = std::move(task_struct);
    *list_value->add_values() = std::move(task_value);
  }
  (*payload_fields)["tasks"] = std::move(tasks_value);

  if (!response.next_page_token.empty()) {
    google::protobuf::Value token_value;
    token_value.set_string_value(response.next_page_token);
    (*payload_fields)["nextPageToken"] = std::move(token_value);
  }

  return core::MessageToJson(payload);
}

core::Result<RestResponse> BuildJsonResponse(const google::protobuf::Message& message) {
  const auto body = core::MessageToJson(message);
  if (!body.ok()) {
    return body.error();
  }

  RestResponse response;
  response.http_status = kHttpOk;
  response.headers["Content-Type"] = "application/json";
  response.body = body.value();
  return response;
}

core::Result<void> AppendSseEvent(RestResponse& response, const lf::a2a::v1::StreamResponse& event) {
  const auto event_json = core::MessageToJson(event);
  if (!event_json.ok()) {
    return event_json.error();
  }
  response.body += "data: ";
  response.body += event_json.value();
  response.body += "\n\n";
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
      return core::Error::Internal("Transport write returned zero bytes while streaming SSE");
    }
    sent += written.value();
  }
  return {};
}

core::Result<RestResponse> BuildStreamingResponse(std::unique_ptr<ServerStreamSession>& session) {
  if (session == nullptr) {
    return core::Error::Internal("Streaming response session is missing");
  }

  RestResponse response;
  response.http_status = kHttpOk;
  response.headers["Content-Type"] = "text/event-stream";
  response.headers["Cache-Control"] = "no-cache";

  auto next = session->Next();
  for (; next.ok(); next = session->Next()) {
    const auto& event = next.value();
    if (!event.has_value()) {
      return response;
    }
    const auto append = AppendSseEvent(response, event.value());
    if (!append.ok()) {
      return append.error();
    }
  }

  return next.error();
}

core::Result<RestResponse> BuildSubscribeResponse(std::unique_ptr<ServerStreamSession>& session) {
  if (session == nullptr) {
    return core::Error::Internal("Subscription response session is missing");
  }

  RestResponse response;
  response.http_status = kHttpOk;
  response.headers["Content-Type"] = "text/event-stream";
  response.headers["Cache-Control"] = "no-cache";
  response.stream_writer = [session = std::make_shared<std::unique_ptr<ServerStreamSession>>(std::move(session))](
                               HttpByteTransport& transport) -> core::Result<void> {
    if (*session == nullptr) {
      return core::Error::Internal("Subscription response session is missing");
    }

    auto next = (*session)->Next();
    for (; next.ok(); next = (*session)->Next()) {
      const auto& event = next.value();
      if (!event.has_value()) {
        return {};
      }
      RestResponse chunk;
      const auto append = AppendSseEvent(chunk, event.value());
      if (!append.ok()) {
        return append.error();
      }
      const auto written = WriteSseChunk(transport, chunk.body);
      if (!written.ok()) {
        return written.error();
      }
    }
    return next.error();
  };

  return response;
}

std::optional<DispatchRequest> BuildMessageDispatchRequest(const RestRequest& request) {
  if (request.method != "POST" ||
      (request.path != RestEndpointPaths::kSendMessage && request.path != RestEndpointPaths::kSendStreamingMessage)) {
    return std::nullopt;
  }

  lf::a2a::v1::SendMessageRequest payload;
  const auto parse = core::JsonToMessage(request.body, &payload, {.ignore_unknown_fields = true});
  if (!parse.ok()) {
    return std::nullopt;
  }

  const DispatcherOperation operation = request.path == RestEndpointPaths::kSendStreamingMessage
                                            ? DispatcherOperation::kSendStreamingMessage
                                            : DispatcherOperation::kSendMessage;
  return DispatchRequest{.operation = operation, .payload = payload};
}

std::optional<DispatchRequest> BuildListTasksDispatchRequest(const RestRequest& request) {
  if (request.method != "GET" || request.path != RestEndpointPaths::kTaskCollection) {
    return std::nullopt;
  }

  ListTasksRequest payload;
  if (const auto raw_page_size = LookupQuery(request, "pageSize"); raw_page_size.has_value()) {
    const int parsed_page_size = ParsePageSize(*raw_page_size);
    if (parsed_page_size < 0) {
      return std::nullopt;
    }
    payload.page_size = static_cast<std::size_t>(parsed_page_size);
  }

  if (const auto raw_page_token = LookupQuery(request, "pageToken"); raw_page_token.has_value()) {
    payload.page_token = *raw_page_token;
  }
  if (const auto context_id = LookupQuery(request, "contextId"); context_id.has_value()) {
    payload.context_id = *context_id;
  }
  if (const auto history_length = LookupQuery(request, "historyLength"); history_length.has_value()) {
    const int parsed_history_length = ParsePageSize(*history_length);
    if (parsed_history_length < 0) {
      return std::nullopt;
    }
    payload.history_length = static_cast<std::size_t>(parsed_history_length);
  }
  if (const auto include_artifacts = LookupQuery(request, "includeArtifacts"); include_artifacts.has_value()) {
    payload.include_artifacts = *include_artifacts == "true";
  }
  return DispatchRequest{.operation = DispatcherOperation::kListTasks, .payload = payload};
}

std::optional<DispatchRequest> BuildGetTaskDispatchRequest(const RestRequest& request) {
  if (request.method != "GET") {
    return std::nullopt;
  }

  const auto task_id = ParseTaskIdFromPath(request.path, false);
  if (!task_id.has_value()) {
    return std::nullopt;
  }

  lf::a2a::v1::GetTaskRequest payload;
  payload.set_id(*task_id);
  if (const auto history_length = LookupQuery(request, "historyLength"); history_length.has_value()) {
    const int parsed_history_length = ParsePageSize(*history_length);
    if (parsed_history_length < 0) {
      return std::nullopt;
    }
    payload.set_history_length(parsed_history_length);
  }
  return DispatchRequest{.operation = DispatcherOperation::kGetTask, .payload = payload};
}

std::optional<DispatchRequest> BuildCancelTaskDispatchRequest(const RestRequest& request) {
  if (request.method != "POST") {
    return std::nullopt;
  }

  const auto task_id = ParseTaskIdFromPath(request.path, true);
  if (!task_id.has_value()) {
    return std::nullopt;
  }

  lf::a2a::v1::CancelTaskRequest payload;
  payload.set_id(*task_id);
  return DispatchRequest{.operation = DispatcherOperation::kCancelTask, .payload = payload};
}

std::optional<DispatchRequest> BuildPushConfigDispatchRequest(const RestRequest& request) {
  const auto path = ParsePushConfigPath(request.path);
  if (!path.has_value()) {
    return std::nullopt;
  }
  if (request.method == "POST" && path->collection) {
    lf::a2a::v1::TaskPushNotificationConfig payload;
    const auto parse = core::JsonToMessage(request.body, &payload, {.ignore_unknown_fields = true});
    if (!parse.ok()) {
      return std::nullopt;
    }
    payload.set_task_id(path->task_id);
    return DispatchRequest{.operation = DispatcherOperation::kCreateTaskPushNotificationConfig, .payload = payload};
  }
  if (request.method == "GET" && path->collection) {
    lf::a2a::v1::ListTaskPushNotificationConfigsRequest payload;
    payload.set_task_id(path->task_id);
    if (const auto page_size = LookupQuery(request, "pageSize"); page_size.has_value()) {
      payload.set_page_size(ParsePageSize(*page_size));
    }
    if (const auto page_token = LookupQuery(request, "pageToken"); page_token.has_value()) {
      payload.set_page_token(*page_token);
    }
    return DispatchRequest{.operation = DispatcherOperation::kListTaskPushNotificationConfigs, .payload = payload};
  }
  if (request.method == "GET" && !path->collection) {
    lf::a2a::v1::GetTaskPushNotificationConfigRequest payload;
    payload.set_task_id(path->task_id);
    payload.set_id(path->config_id);
    return DispatchRequest{.operation = DispatcherOperation::kGetTaskPushNotificationConfig, .payload = payload};
  }
  if (request.method == "DELETE" && !path->collection) {
    lf::a2a::v1::DeleteTaskPushNotificationConfigRequest payload;
    payload.set_task_id(path->task_id);
    payload.set_id(path->config_id);
    return DispatchRequest{.operation = DispatcherOperation::kDeleteTaskPushNotificationConfig, .payload = payload};
  }
  return std::nullopt;
}

}  // namespace

RestTransport::RestTransport(Dispatcher* dispatcher) : dispatcher_(dispatcher) {}

const std::vector<RestRoute>& RestTransport::Routes() {
  static const auto* routes = new std::vector<RestRoute>(kRoutes.begin(), kRoutes.end());
  return *routes;
}

std::optional<DispatchRequest> RestTransport::BuildDispatchRequest(const RestRequest& request) {
  if (auto dispatch_request = BuildPushConfigDispatchRequest(request); dispatch_request.has_value()) {
    return dispatch_request;
  }
  if (auto dispatch_request = BuildMessageDispatchRequest(request); dispatch_request.has_value()) {
    return dispatch_request;
  }
  if (auto dispatch_request = BuildListTasksDispatchRequest(request); dispatch_request.has_value()) {
    return dispatch_request;
  }
  if (auto dispatch_request = BuildGetTaskDispatchRequest(request); dispatch_request.has_value()) {
    return dispatch_request;
  }
  return BuildCancelTaskDispatchRequest(request);
}

core::Result<RestResponse> RestTransport::SerializeDispatchResponse(DispatcherOperation operation,
                                                                    DispatchResponse& response) {
  switch (operation) {
    case DispatcherOperation::kSendMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageResponse>(&response.payload());
      if (payload == nullptr) {
        return InternalResponsePayloadMismatch(core::protocol_error_messages::kResponsePayloadMismatchForSendMessage);
      }
      return BuildJsonResponse(*payload);
    }
    case DispatcherOperation::kSendStreamingMessage: {
      auto* payload = std::get_if<std::unique_ptr<ServerStreamSession>>(&response.payload());
      if (payload == nullptr) {
        return InternalResponsePayloadMismatch(
            core::protocol_error_messages::kResponsePayloadMismatchForSendStreamingMessage);
      }
      return BuildStreamingResponse(*payload);
    }
    case DispatcherOperation::kSubscribeTask: {
      auto* payload = std::get_if<std::unique_ptr<ServerStreamSession>>(&response.payload());
      if (payload == nullptr) {
        return InternalResponsePayloadMismatch(
            core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForSubscribeToTask);
      }
      return BuildSubscribeResponse(*payload);
    }
    case DispatcherOperation::kGetTask:
    case DispatcherOperation::kCancelTask: {
      const auto* payload = std::get_if<lf::a2a::v1::Task>(&response.payload());
      if (payload == nullptr) {
        return InternalResponsePayloadMismatch(core::protocol_error_messages::kResponsePayloadMismatchForTask);
      }
      return BuildJsonResponse(*payload);
    }
    case DispatcherOperation::kCreateTaskPushNotificationConfig:
    case DispatcherOperation::kGetTaskPushNotificationConfig: {
      const auto* payload = std::get_if<lf::a2a::v1::TaskPushNotificationConfig>(&response.payload());
      if (payload == nullptr) {
        return InternalResponsePayloadMismatch(core::protocol_error_messages::kResponsePayloadMismatchForPushConfig);
      }
      return BuildJsonResponse(*payload);
    }
    case DispatcherOperation::kListTaskPushNotificationConfigs: {
      const auto* payload = std::get_if<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(&response.payload());
      if (payload == nullptr) {
        return InternalResponsePayloadMismatch(
            core::protocol_error_messages::kResponsePayloadMismatchForPushConfigList);
      }
      return BuildJsonResponse(*payload);
    }
    case DispatcherOperation::kDeleteTaskPushNotificationConfig: {
      google::protobuf::Struct empty;
      return BuildJsonResponse(empty);
    }
    case DispatcherOperation::kListTasks: {
      const auto* payload = std::get_if<ListTasksResponse>(&response.payload());
      if (payload == nullptr) {
        return InternalResponsePayloadMismatch(core::protocol_error_messages::kResponsePayloadMismatchForListTasks);
      }
      const auto body = BuildListTasksJson(*payload);
      if (!body.ok()) {
        return body.error();
      }
      RestResponse rest_response;
      rest_response.http_status = kHttpOk;
      rest_response.headers["Content-Type"] = "application/json";
      rest_response.body = body.value();
      return rest_response;
    }
  }

  return core::Error::Internal("Unsupported dispatcher operation");
}

RestResponse RestTransport::BuildErrorResponse(const core::Error& error) {
  const int http_status = ToHttpStatus(error);

  google::protobuf::Struct error_info;
  auto* error_info_fields = error_info.mutable_fields();
  (*error_info_fields)["@type"].set_string_value("type.googleapis.com/google.rpc.ErrorInfo");
  (*error_info_fields)["reason"].set_string_value(ErrorInfoReason(error));
  (*error_info_fields)["domain"].set_string_value("a2a-protocol.org");

  const auto& transport_value = error.transport();
  const auto& protocol_code_value = error.protocol_code();
  if (transport_value.has_value() || protocol_code_value.has_value()) {
    google::protobuf::Value metadata_value;
    auto* metadata_fields = metadata_value.mutable_struct_value()->mutable_fields();
    if (transport_value.has_value()) {
      (*metadata_fields)["transport"].set_string_value(*transport_value);
    }
    if (protocol_code_value.has_value()) {
      (*metadata_fields)["protocolCode"].set_string_value(*protocol_code_value);
    }
    (*error_info_fields)["metadata"] = std::move(metadata_value);
  }

  google::protobuf::Struct envelope;
  auto* envelope_fields = envelope.mutable_fields();
  google::protobuf::Value error_node;
  auto* error_fields = error_node.mutable_struct_value()->mutable_fields();
  (*error_fields)["code"].set_number_value(http_status);
  (*error_fields)["status"].set_string_value(ErrorStatusName(http_status));
  (*error_fields)["message"].set_string_value(std::string(error.message()));

  google::protobuf::Value details;
  auto* details_values = details.mutable_list_value()->mutable_values();
  google::protobuf::Value error_info_value;
  *error_info_value.mutable_struct_value() = std::move(error_info);
  details_values->Add(std::move(error_info_value));
  (*error_fields)["details"] = std::move(details);

  (*envelope_fields)["error"] = std::move(error_node);

  RestResponse response;
  response.http_status = http_status;
  response.headers["Content-Type"] = "application/json";

  const auto serialized = core::MessageToJson(envelope);
  if (serialized.ok()) {
    response.body = serialized.value();
  } else {
    response.body = R"({"error":{"code":500,"status":"INTERNAL","message":"Failed to serialize error"}})";
  }
  return response;
}

core::Result<RestResponse> RestTransport::Handle(const RestRequest& request) const {
  if (dispatcher_ == nullptr) {
    return core::Error::Internal("REST transport dispatcher is not configured");
  }

  if (request.method == "GET") {
    const auto subscribe_task_id = ParseTaskIdFromActionPath(request.path, kTaskSubscribeSuffix);
    if (subscribe_task_id.has_value()) {
      lf::a2a::v1::GetTaskRequest get_task_request;
      get_task_request.set_id(*subscribe_task_id);
      RequestContext context = request.context;
      auto dispatch_response = dispatcher_->Dispatch(
          {.operation = DispatcherOperation::kSubscribeTask, .payload = get_task_request}, context);
      if (!dispatch_response.ok()) {
        return BuildErrorResponse(dispatch_response.error().WithTransport("rest"));
      }
      auto* stream = std::get_if<std::unique_ptr<ServerStreamSession>>(&dispatch_response.value().payload());
      if (stream == nullptr || *stream == nullptr) {
        return BuildErrorResponse(
            core::Error::Internal(core::protocol_error_messages::ToString(
                                      core::protocol_error_messages::kUnexpectedDispatchPayloadTypeForSubscribeToTask))
                .WithTransport("rest"));
      }
      const auto subscribe_response = BuildSubscribeResponse(*stream);
      if (!subscribe_response.ok()) {
        return BuildErrorResponse(subscribe_response.error().WithTransport("rest"));
      }
      return subscribe_response.value();
    }
  }

  const auto dispatch_request = BuildDispatchRequest(request);
  if (!dispatch_request.has_value()) {
    return BuildErrorResponse(
        core::Error::Validation("No matching route or request was malformed").WithHttpStatus(kHttpNotFound));
  }

  RequestContext context = request.context;
  auto dispatch_response = dispatcher_->Dispatch(dispatch_request.value(), context);
  if (!dispatch_response.ok()) {
    return BuildErrorResponse(dispatch_response.error().WithTransport("rest"));
  }

  const auto response = SerializeDispatchResponse(dispatch_request->operation, dispatch_response.value());
  if (!response.ok()) {
    return BuildErrorResponse(response.error().WithTransport("rest"));
  }

  return response.value();
}

}  // namespace a2a::server
