#include "a2a/server/rest_transport.h"

#include <google/protobuf/struct.pb.h>

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/protojson.h"
#include "a2a/core/task_states.h"

namespace a2a::server {
namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpConflict = 409;
constexpr int kHttpNotFound = 404;
constexpr int kHttpBadGateway = 502;
constexpr int kHttpServiceUnavailable = 503;
constexpr int kHttpInternalServerError = 500;
constexpr std::size_t kDecimalBase = 10;
constexpr std::string_view kTaskSubscribeSuffix = ":subscribe";
constexpr std::string_view kPushNotificationConfigsSegment = "/pushNotificationConfigs";

const std::array<RestRoute, 6> kRoutes = {
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
    RestRoute{.method = "POST", .path_pattern = "/tasks/{id}:subscribe", .operation = DispatcherOperation::kGetTask},
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
  const std::string task_id(path.substr(id_start, id_length));
  if (task_id.empty() || task_id.find('/') != std::string::npos) {
    return std::nullopt;
  }
  return task_id;
}

bool IsPushNotificationConfigPath(const RestRequest& request) {
  if (request.method != "POST" && request.method != "GET" && request.method != "DELETE") {
    return false;
  }
  if (!request.path.starts_with(RestEndpointPaths::kTaskResourcePrefix)) {
    return false;
  }
  const auto suffix = request.path.substr(RestEndpointPaths::kTaskResourcePrefix.size());
  const auto push_segment = suffix.find(kPushNotificationConfigsSegment);
  return push_segment != std::string_view::npos && push_segment > 0;
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
      return "INTERNAL";
    case kHttpServiceUnavailable:
      return "UNAVAILABLE";
    case kHttpInternalServerError:
      return "INTERNAL";
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

  switch (error.code()) {
    case core::ErrorCode::kUnsupportedVersion:
      return "VERSION_NOT_SUPPORTED";
    case core::ErrorCode::kRemoteProtocol:
      return "UNSUPPORTED_OPERATION";
    case core::ErrorCode::kValidation:
      return "UNSUPPORTED_OPERATION";
    case core::ErrorCode::kNetwork:
    case core::ErrorCode::kSerialization:
    case core::ErrorCode::kInternal:
      return "INVALID_AGENT_RESPONSE";
  }
  return "INVALID_AGENT_RESPONSE";
}

int ToHttpStatus(const core::Error& error) {
  const auto& http_status = error.http_status();
  if (http_status.has_value()) {
    return *http_status;
  }
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return kHttpBadRequest;
    case core::ErrorCode::kUnsupportedVersion:
      return kHttpBadRequest;
    case core::ErrorCode::kNetwork:
      return kHttpServiceUnavailable;
    case core::ErrorCode::kRemoteProtocol:
      return kHttpBadGateway;
    case core::ErrorCode::kSerialization:
    case core::ErrorCode::kInternal:
      return kHttpInternalServerError;
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

core::Result<RestResponse> BuildStreamingResponse(std::unique_ptr<ServerStreamSession>& session) {
  if (session == nullptr) {
    return core::Error::Internal("Streaming response session is missing");
  }

  RestResponse response;
  response.http_status = kHttpOk;
  response.headers["Content-Type"] = "text/event-stream";
  response.headers["Cache-Control"] = "no-cache";

  while (true) {
    auto next = session->Next();
    if (!next.ok()) {
      return next.error();
    }
    if (!next.value().has_value()) {
      break;
    }
    const auto append = AppendSseEvent(response, *next.value());
    if (!append.ok()) {
      return append.error();
    }
  }

  return response;
}

core::Result<RestResponse> BuildSubscribeResponse(const lf::a2a::v1::Task& task) {
  if (core::IsTerminalTaskState(task.status().state())) {
    return core::protocol_errors::UnsupportedOperation("task is already terminal");
  }

  RestResponse response;
  response.http_status = kHttpOk;
  response.headers["Content-Type"] = "text/event-stream";
  response.headers["Cache-Control"] = "no-cache";

  lf::a2a::v1::StreamResponse current_event;
  *current_event.mutable_task() = task;
  const auto current_append = AppendSseEvent(response, current_event);
  if (!current_append.ok()) {
    return current_append.error();
  }

  lf::a2a::v1::StreamResponse terminal_event;
  terminal_event.mutable_status_update()->set_task_id(task.id());
  terminal_event.mutable_status_update()->set_context_id(task.context_id());
  terminal_event.mutable_status_update()->mutable_status()->set_state(lf::a2a::v1::TASK_STATE_COMPLETED);
  const auto terminal_append = AppendSseEvent(response, terminal_event);
  if (!terminal_append.ok()) {
    return terminal_append.error();
  }

  return response;
}

}  // namespace

RestTransport::RestTransport(Dispatcher* dispatcher) : dispatcher_(dispatcher) {}

const std::vector<RestRoute>& RestTransport::Routes() {
  static const auto* routes = new std::vector<RestRoute>(kRoutes.begin(), kRoutes.end());
  return *routes;
}

std::optional<DispatchRequest> RestTransport::BuildDispatchRequest(const RestRequest& request) {
  if (request.method == "POST" &&
      (request.path == RestEndpointPaths::kSendMessage || request.path == RestEndpointPaths::kSendStreamingMessage)) {
    lf::a2a::v1::SendMessageRequest payload;
    const auto parse = core::JsonToMessage(request.body, &payload);
    if (!parse.ok()) {
      return std::nullopt;
    }
    const DispatcherOperation operation = request.path == RestEndpointPaths::kSendStreamingMessage
                                              ? DispatcherOperation::kSendStreamingMessage
                                              : DispatcherOperation::kSendMessage;
    return DispatchRequest{.operation = operation, .payload = payload};
  }

  if (request.method == "GET" && request.path == RestEndpointPaths::kTaskCollection) {
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

  if (request.method == "GET") {
    const auto task_id = ParseTaskIdFromPath(request.path, false);
    if (task_id.has_value()) {
      lf::a2a::v1::GetTaskRequest payload;
      payload.set_id(task_id.value());
      if (const auto history_length = LookupQuery(request, "historyLength"); history_length.has_value()) {
        const int parsed_history_length = ParsePageSize(*history_length);
        if (parsed_history_length < 0) {
          return std::nullopt;
        }
        payload.set_history_length(parsed_history_length);
      }
      return DispatchRequest{.operation = DispatcherOperation::kGetTask, .payload = payload};
    }
  }

  if (request.method == "POST") {
    const auto task_id = ParseTaskIdFromPath(request.path, true);
    if (task_id.has_value()) {
      lf::a2a::v1::CancelTaskRequest payload;
      payload.set_id(task_id.value());
      return DispatchRequest{.operation = DispatcherOperation::kCancelTask, .payload = payload};
    }
  }

  return std::nullopt;
}

core::Result<RestResponse> RestTransport::SerializeDispatchResponse(DispatcherOperation operation,
                                                                    DispatchResponse& response) {
  switch (operation) {
    case DispatcherOperation::kSendMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageResponse>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("SendMessage response payload mismatch");
      }
      return BuildJsonResponse(*payload);
    }
    case DispatcherOperation::kSendStreamingMessage: {
      auto* payload = std::get_if<std::unique_ptr<ServerStreamSession>>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("SendStreamingMessage response payload mismatch");
      }
      return BuildStreamingResponse(*payload);
    }
    case DispatcherOperation::kGetTask:
    case DispatcherOperation::kCancelTask: {
      const auto* payload = std::get_if<lf::a2a::v1::Task>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("Task response payload mismatch");
      }
      return BuildJsonResponse(*payload);
    }
    case DispatcherOperation::kListTasks: {
      const auto* payload = std::get_if<ListTasksResponse>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("ListTasks response payload mismatch");
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

  if (IsPushNotificationConfigPath(request)) {
    return BuildErrorResponse(core::protocol_errors::PushNotificationNotSupported().WithTransport("rest"));
  }

  if (request.method == "POST") {
    const auto subscribe_task_id = ParseTaskIdFromActionPath(request.path, kTaskSubscribeSuffix);
    if (subscribe_task_id.has_value()) {
      lf::a2a::v1::GetTaskRequest get_task_request;
      get_task_request.set_id(*subscribe_task_id);
      RequestContext context = request.context;
      const auto dispatch_response =
          dispatcher_->Dispatch({.operation = DispatcherOperation::kGetTask, .payload = get_task_request}, context);
      if (!dispatch_response.ok()) {
        return BuildErrorResponse(dispatch_response.error().WithTransport("rest"));
      }
      const auto* task = std::get_if<lf::a2a::v1::Task>(&dispatch_response.value().payload());
      if (task == nullptr) {
        return BuildErrorResponse(
            core::Error::Internal("Unexpected dispatch payload type for SubscribeToTask").WithTransport("rest"));
      }
      const auto subscribe_response = BuildSubscribeResponse(*task);
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
