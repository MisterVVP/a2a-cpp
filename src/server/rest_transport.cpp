#include "a2a/server/rest_transport.h"

#include <google/protobuf/struct.pb.h>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protojson.h"

namespace a2a::server {
namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpNotFound = 404;
constexpr int kHttpUpgradeRequired = 426;
constexpr int kHttpBadGateway = 502;
constexpr int kHttpServiceUnavailable = 503;
constexpr int kHttpInternalServerError = 500;
constexpr std::size_t kDecimalBase = 10;

const std::array<RestRoute, 4> kRoutes = {
    RestRoute{.method = "POST",
              .path_pattern = RestEndpointPaths::kSendMessage,
              .operation = DispatcherOperation::kSendMessage},
    RestRoute{
        .method = "GET", .path_pattern = "/tasks/{id}", .operation = DispatcherOperation::kGetTask},
    RestRoute{.method = "GET",
              .path_pattern = RestEndpointPaths::kTaskCollection,
              .operation = DispatcherOperation::kListTasks},
    RestRoute{.method = "POST",
              .path_pattern = "/tasks/{id}:cancel",
              .operation = DispatcherOperation::kCancelTask},
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
  } else if (suffix.ends_with(RestEndpointPaths::kTaskCancelSuffix)) {
    return std::nullopt;
  }

  if (suffix.empty() || suffix.find('/') != std::string::npos) {
    return std::nullopt;
  }

  return suffix;
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

std::string ErrorCodeName(core::ErrorCode code) {
  switch (code) {
    case core::ErrorCode::kValidation:
      return "validation_error";
    case core::ErrorCode::kUnsupportedVersion:
      return "unsupported_version";
    case core::ErrorCode::kNetwork:
      return "network_error";
    case core::ErrorCode::kRemoteProtocol:
      return "remote_protocol_error";
    case core::ErrorCode::kSerialization:
      return "serialization_error";
    case core::ErrorCode::kInternal:
      return "internal_error";
  }
  return "internal_error";
}

int ToHttpStatus(const core::Error& error) {
  if (error.http_status().has_value()) {
    return error.http_status().value();
  }
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return kHttpBadRequest;
    case core::ErrorCode::kUnsupportedVersion:
      return kHttpUpgradeRequired;
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

}  // namespace

RestTransport::RestTransport(Dispatcher* dispatcher) : dispatcher_(dispatcher) {}

const std::vector<RestRoute>& RestTransport::Routes() {
  static const auto* routes = new std::vector<RestRoute>(kRoutes.begin(), kRoutes.end());
  return *routes;
}

std::optional<DispatchRequest> RestTransport::BuildDispatchRequest(const RestRequest& request) {
  if (request.method == "POST" && request.path == RestEndpointPaths::kSendMessage) {
    lf::a2a::v1::SendMessageRequest payload;
    const auto parse = core::JsonToMessage(request.body, &payload);
    if (!parse.ok()) {
      return std::nullopt;
    }
    return DispatchRequest{.operation = DispatcherOperation::kSendMessage, .payload = payload};
  }

  if (request.method == "GET" && request.path == RestEndpointPaths::kTaskCollection) {
    ListTasksRequest payload;
    const auto raw_page_size = LookupQuery(request, "pageSize");
    if (raw_page_size.has_value()) {
      const int parsed_page_size = ParsePageSize(raw_page_size.value());
      if (parsed_page_size < 0) {
        return std::nullopt;
      }
      payload.page_size = static_cast<std::size_t>(parsed_page_size);
    }

    const auto raw_page_token = LookupQuery(request, "pageToken");
    if (raw_page_token.has_value()) {
      payload.page_token = raw_page_token.value();
    }
    return DispatchRequest{.operation = DispatcherOperation::kListTasks, .payload = payload};
  }

  if (request.method == "GET") {
    const auto task_id = ParseTaskIdFromPath(request.path, false);
    if (task_id.has_value()) {
      lf::a2a::v1::GetTaskRequest payload;
      payload.set_id(task_id.value());
      const auto history_length = LookupQuery(request, "historyLength");
      if (history_length.has_value()) {
        payload.set_history_length(history_length.value());
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

core::Result<std::string> RestTransport::SerializeDispatchResponse(
    DispatcherOperation operation, const DispatchResponse& response) {
  switch (operation) {
    case DispatcherOperation::kSendMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageResponse>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("SendMessage response payload mismatch");
      }
      return core::MessageToJson(*payload);
    }
    case DispatcherOperation::kGetTask:
    case DispatcherOperation::kCancelTask: {
      const auto* payload = std::get_if<lf::a2a::v1::Task>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("Task response payload mismatch");
      }
      return core::MessageToJson(*payload);
    }
    case DispatcherOperation::kListTasks: {
      const auto* payload = std::get_if<ListTasksResponse>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("ListTasks response payload mismatch");
      }
      return BuildListTasksJson(*payload);
    }
    case DispatcherOperation::kSendStreamingMessage:
      return core::Error::Validation("Streaming route is not supported by REST transport adapter");
  }

  return core::Error::Internal("Unsupported dispatcher operation");
}

RestResponse RestTransport::BuildErrorResponse(const core::Error& error) {
  google::protobuf::Struct details;
  auto* detail_fields = details.mutable_fields();

  google::protobuf::Value code_name;
  code_name.set_string_value(ErrorCodeName(error.code()));
  (*detail_fields)["code"] = std::move(code_name);

  if (error.transport().has_value()) {
    google::protobuf::Value transport;
    transport.set_string_value(error.transport().value());
    (*detail_fields)["transport"] = std::move(transport);
  }

  if (error.protocol_code().has_value()) {
    google::protobuf::Value protocol_code;
    protocol_code.set_string_value(error.protocol_code().value());
    (*detail_fields)["protocolCode"] = std::move(protocol_code);
  }

  google::protobuf::Struct envelope;
  auto* envelope_fields = envelope.mutable_fields();
  google::protobuf::Value error_node;
  auto* error_fields = error_node.mutable_struct_value()->mutable_fields();

  google::protobuf::Value message;
  message.set_string_value(std::string(error.message()));
  (*error_fields)["message"] = std::move(message);

  google::protobuf::Value detail_struct;
  *detail_struct.mutable_struct_value() = std::move(details);
  (*error_fields)["details"] = std::move(detail_struct);

  (*envelope_fields)["error"] = std::move(error_node);

  RestResponse response;
  response.http_status = ToHttpStatus(error);
  response.headers["Content-Type"] = "application/json";

  const auto serialized = core::MessageToJson(envelope);
  if (serialized.ok()) {
    response.body = serialized.value();
  } else {
    response.body = R"({"error":{"message":"Failed to serialize error"}})";
  }
  return response;
}

core::Result<RestResponse> RestTransport::Handle(const RestRequest& request) const {
  if (dispatcher_ == nullptr) {
    return core::Error::Internal("REST transport dispatcher is not configured");
  }

  const auto dispatch_request = BuildDispatchRequest(request);
  if (!dispatch_request.has_value()) {
    return BuildErrorResponse(core::Error::Validation("No matching route or request was malformed")
                                  .WithHttpStatus(kHttpNotFound));
  }

  RequestContext context = request.context;
  const auto dispatch_response = dispatcher_->Dispatch(dispatch_request.value(), context);
  if (!dispatch_response.ok()) {
    return BuildErrorResponse(dispatch_response.error().WithTransport("rest"));
  }

  const auto body =
      SerializeDispatchResponse(dispatch_request->operation, dispatch_response.value());
  if (!body.ok()) {
    return BuildErrorResponse(body.error().WithTransport("rest"));
  }

  RestResponse response;
  response.http_status = kHttpOk;
  response.headers["Content-Type"] = "application/json";
  response.body = body.value();
  return response;
}

}  // namespace a2a::server
