#include "a2a/server/json_rpc_server_transport.h"

#include <google/protobuf/struct.pb.h>

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/json_rpc.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::server {
namespace {

constexpr int kHttpOk = 200;
constexpr int kHttpBadRequest = 400;
constexpr int kHttpUpgradeRequired = 426;
constexpr int kHttpInternalServerError = 500;

constexpr int kJsonRpcParseError = -32700;
constexpr int kJsonRpcInvalidRequest = -32600;
constexpr int kJsonRpcMethodNotFound = -32601;
constexpr int kJsonRpcInvalidParams = -32602;
constexpr int kJsonRpcInternalError = -32603;

std::string ToLower(std::string_view value) {
  std::string lowered;
  lowered.reserve(value.size());
  for (const auto ch : value) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return lowered;
}

std::string FindHeader(const std::unordered_map<std::string, std::string>& headers,
                       std::string_view name) {
  const std::string lowered_name = ToLower(name);
  for (const auto& [header_name, header_value] : headers) {
    if (ToLower(header_name) == lowered_name) {
      return header_value;
    }
  }
  return {};
}

bool IsValidIdType(const google::protobuf::Value& value) {
  return value.has_null_value() || value.has_string_value() || value.has_number_value();
}

std::optional<DispatcherOperation> MethodToOperation(std::string_view method) {
  if (method == core::json_rpc::MethodNames::kSendMessage) {
    return DispatcherOperation::kSendMessage;
  }
  if (method == core::json_rpc::MethodNames::kGetTask) {
    return DispatcherOperation::kGetTask;
  }
  if (method == core::json_rpc::MethodNames::kCancelTask) {
    return DispatcherOperation::kCancelTask;
  }
  if (method == core::json_rpc::MethodNames::kListTasks) {
    return DispatcherOperation::kListTasks;
  }
  return std::nullopt;
}

core::Result<google::protobuf::Value> BuildJsonValueFromMessage(
    const google::protobuf::Message& message) {
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

  if (!list_response.next_page_token.empty()) {
    google::protobuf::Value token;
    token.set_string_value(list_response.next_page_token);
    (*fields)["nextPageToken"] = std::move(token);
  }

  google::protobuf::Value wrapper;
  *wrapper.mutable_struct_value() = std::move(result);
  return wrapper;
}

int HttpStatusFromError(const core::Error& error) {
  if (error.http_status().has_value()) {
    return error.http_status().value();
  }
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return kHttpBadRequest;
    case core::ErrorCode::kUnsupportedVersion:
      return kHttpUpgradeRequired;
    case core::ErrorCode::kNetwork:
    case core::ErrorCode::kRemoteProtocol:
    case core::ErrorCode::kSerialization:
    case core::ErrorCode::kInternal:
      return kHttpInternalServerError;
  }
  return kHttpInternalServerError;
}

int JsonRpcCodeFromError(const core::Error& error) {
  switch (error.code()) {
    case core::ErrorCode::kValidation:
      return kJsonRpcInvalidParams;
    case core::ErrorCode::kUnsupportedVersion:
      return kJsonRpcInvalidRequest;
    case core::ErrorCode::kNetwork:
    case core::ErrorCode::kRemoteProtocol:
    case core::ErrorCode::kSerialization:
    case core::ErrorCode::kInternal:
      return kJsonRpcInternalError;
  }
  return kJsonRpcInternalError;
}

}  // namespace

JsonRpcServerTransport::JsonRpcServerTransport(Dispatcher* dispatcher,
                                               JsonRpcServerTransportOptions options)
    : dispatcher_(dispatcher), options_(std::move(options)) {
  options_.rpc_path = NormalizePath(std::move(options_.rpc_path));
}

core::Result<HttpServerResponse> JsonRpcServerTransport::Handle(
    const HttpServerRequest& request) const {
  if (dispatcher_ == nullptr) {
    return core::Error::Internal("JSON-RPC server dispatcher is not configured");
  }

  if (request.method != "POST" || request.target != options_.rpc_path) {
    return BuildErrorResponse(kJsonRpcInvalidRequest, "No matching JSON-RPC route", ResponseId{},
                              std::nullopt, kHttpBadRequest);
  }

  const auto version = ValidateVersionHeader(request);
  if (!version.ok()) {
    return BuildErrorResponse(kJsonRpcInvalidRequest, version.error().message(), ResponseId{},
                              version.error(), kHttpUpgradeRequired);
  }

  const auto parsed = ParseRequest(request.body);
  if (!parsed.ok()) {
    const int parse_code = parsed.error().code() == core::ErrorCode::kSerialization
                               ? kJsonRpcParseError
                               : kJsonRpcInvalidRequest;
    return BuildErrorResponse(parse_code, parsed.error().message(), ResponseId{}, parsed.error(),
                              kHttpBadRequest);
  }

  RequestContext context;
  context.remote_address = request.remote_address.empty()
                               ? std::optional<std::string>{}
                               : std::optional<std::string>(request.remote_address);
  context.client_headers = request.headers;

  const auto dispatch = dispatcher_->Dispatch(parsed.value().dispatch, context);
  if (!dispatch.ok()) {
    const int http_status = HttpStatusFromError(dispatch.error());
    return BuildErrorResponse(JsonRpcCodeFromError(dispatch.error()), dispatch.error().message(),
                              parsed.value().id, dispatch.error().WithTransport("jsonrpc"),
                              http_status);
  }

  const auto result = SerializeDispatchResult(parsed.value().dispatch, dispatch.value());
  if (!result.ok()) {
    const auto tagged = result.error().WithTransport("jsonrpc");
    return BuildErrorResponse(JsonRpcCodeFromError(tagged), tagged.message(), parsed.value().id,
                              tagged, HttpStatusFromError(tagged));
  }

  return BuildSuccessResponse(parsed.value().id, result.value());
}

core::Result<void> JsonRpcServerTransport::ValidateVersionHeader(
    const HttpServerRequest& request) const {
  const std::string version = FindHeader(request.headers, core::Version::kHeaderName);
  if (version.empty()) {
    if (options_.require_version_header) {
      return core::Error::UnsupportedVersion("Missing required A2A-Version header");
    }
    return {};
  }

  if (!core::Version::IsSupported(version)) {
    return core::Error::UnsupportedVersion("Unsupported A2A-Version header value")
        .WithProtocolCode(version);
  }

  return {};
}

core::Result<JsonRpcServerTransport::JsonRpcRequest> JsonRpcServerTransport::ParseRequest(
    std::string_view body) const {
  google::protobuf::Struct envelope;
  const auto parsed = core::JsonToMessage(body, &envelope);
  if (!parsed.ok()) {
    return parsed.error();
  }

  const auto& fields = envelope.fields();
  const auto version_it = fields.find("jsonrpc");
  if (version_it == fields.end() || !version_it->second.has_string_value() ||
      version_it->second.string_value() != core::json_rpc::kVersion) {
    return core::Error::Validation("JSON-RPC request has invalid version");
  }

  const auto id_it = fields.find("id");
  if (id_it == fields.end() || !IsValidIdType(id_it->second)) {
    return core::Error::Validation("JSON-RPC request id must be a string, number, or null");
  }

  const auto method_it = fields.find("method");
  if (method_it == fields.end() || !method_it->second.has_string_value() ||
      method_it->second.string_value().empty()) {
    return core::Error::Validation("JSON-RPC request method must be a non-empty string");
  }

  const auto params_it = fields.find("params");
  if (params_it == fields.end() || !params_it->second.has_struct_value()) {
    return core::Error::Validation("JSON-RPC request params must be an object");
  }

  const auto operation = MethodToOperation(method_it->second.string_value());
  if (!operation.has_value()) {
    return core::Error::RemoteProtocol("JSON-RPC method is not supported")
        .WithProtocolCode(std::to_string(kJsonRpcMethodNotFound));
  }

  const auto params_json = core::MessageToJson(params_it->second);
  if (!params_json.ok()) {
    return params_json.error();
  }

  DispatchRequest dispatch_request;
  dispatch_request.operation = operation.value();

  switch (dispatch_request.operation) {
    case DispatcherOperation::kSendMessage: {
      lf::a2a::v1::SendMessageRequest payload;
      const auto parse_payload = core::JsonToMessage(params_json.value(), &payload);
      if (!parse_payload.ok()) {
        return parse_payload.error();
      }
      dispatch_request.payload = std::move(payload);
      break;
    }
    case DispatcherOperation::kGetTask: {
      lf::a2a::v1::GetTaskRequest payload;
      const auto parse_payload = core::JsonToMessage(params_json.value(), &payload);
      if (!parse_payload.ok()) {
        return parse_payload.error();
      }
      dispatch_request.payload = std::move(payload);
      break;
    }
    case DispatcherOperation::kCancelTask: {
      lf::a2a::v1::CancelTaskRequest payload;
      const auto parse_payload = core::JsonToMessage(params_json.value(), &payload);
      if (!parse_payload.ok()) {
        return parse_payload.error();
      }
      dispatch_request.payload = std::move(payload);
      break;
    }
    case DispatcherOperation::kListTasks: {
      google::protobuf::Struct params = params_it->second.struct_value();
      ListTasksRequest payload;

      const auto page_size_it = params.fields().find("pageSize");
      if (page_size_it != params.fields().end()) {
        if (!page_size_it->second.has_number_value() || page_size_it->second.number_value() < 0) {
          return core::Error::Validation("ListTasksRequest.pageSize must be a non-negative number");
        }
        payload.page_size = static_cast<std::size_t>(page_size_it->second.number_value());
      }

      const auto page_token_it = params.fields().find("pageToken");
      if (page_token_it != params.fields().end()) {
        if (!page_token_it->second.has_string_value()) {
          return core::Error::Validation("ListTasksRequest.pageToken must be a string");
        }
        payload.page_token = page_token_it->second.string_value();
      }

      dispatch_request.payload = std::move(payload);
      break;
    }
    case DispatcherOperation::kSendStreamingMessage:
      return core::Error::Validation("Streaming JSON-RPC route is not supported");
  }

  return JsonRpcRequest{.id = ResponseId(id_it->second), .dispatch = std::move(dispatch_request)};
}

core::Result<google::protobuf::Value> JsonRpcServerTransport::SerializeDispatchResult(
    const DispatchRequest& request, const DispatchResponse& response) {
  switch (request.operation) {
    case DispatcherOperation::kSendMessage: {
      const auto* payload = std::get_if<lf::a2a::v1::SendMessageResponse>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("JSON-RPC SendMessage response payload mismatch");
      }
      return BuildJsonValueFromMessage(*payload);
    }
    case DispatcherOperation::kGetTask:
    case DispatcherOperation::kCancelTask: {
      const auto* payload = std::get_if<lf::a2a::v1::Task>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("JSON-RPC Task response payload mismatch");
      }
      return BuildJsonValueFromMessage(*payload);
    }
    case DispatcherOperation::kListTasks: {
      const auto* payload = std::get_if<ListTasksResponse>(&response.payload());
      if (payload == nullptr) {
        return core::Error::Internal("JSON-RPC ListTasks response payload mismatch");
      }
      return BuildListTasksResult(*payload);
    }
    case DispatcherOperation::kSendStreamingMessage:
      return core::Error::Validation("Streaming JSON-RPC route is not supported");
  }

  return core::Error::Internal("Unsupported JSON-RPC dispatcher operation");
}

HttpServerResponse JsonRpcServerTransport::BuildSuccessResponse(
    const ResponseId& id, const google::protobuf::Value& result) {
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
    response.body =
        R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"Failed to serialize response"}})";
    response.status_code = kHttpInternalServerError;
  }

  return response;
}

HttpServerResponse JsonRpcServerTransport::BuildErrorResponse(
    int json_rpc_code, std::string_view message, const ResponseId& id,
    const std::optional<core::Error>& error, int http_status) {
  google::protobuf::Struct envelope;
  auto* fields = envelope.mutable_fields();
  (*fields)["jsonrpc"].set_string_value(std::string(core::json_rpc::kVersion));
  (*fields)["id"] = id.value();

  google::protobuf::Value error_value;
  auto* error_fields = error_value.mutable_struct_value()->mutable_fields();
  (*error_fields)["code"].set_number_value(json_rpc_code);
  (*error_fields)["message"].set_string_value(std::string(message));

  if (error.has_value()) {
    google::protobuf::Value data;
    auto* data_fields = data.mutable_struct_value()->mutable_fields();
    (*data_fields)["a2aCode"].set_string_value(
        std::to_string(static_cast<std::int32_t>(error->code())));
    if (error->protocol_code().has_value()) {
      (*data_fields)["protocolCode"].set_string_value(error->protocol_code().value());
    }
    if (error->transport().has_value()) {
      (*data_fields)["transport"].set_string_value(error->transport().value());
    }
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
    response.body =
        R"({"jsonrpc":"2.0","id":null,"error":{"code":-32603,"message":"Failed to serialize error"}})";
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
