#include "a2a/client/json_rpc_transport.h"

#include <google/protobuf/empty.pb.h>
#include <google/protobuf/struct.pb.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::client {
namespace {

constexpr int kHttpOkMin = 200;
constexpr int kHttpOkMax = 299;
constexpr std::string_view kJsonRpcVersion = "2.0";

struct JsonRpcMethodMap final {
  static constexpr std::string_view kSendMessage = "a2a.sendMessage";
  static constexpr std::string_view kGetTask = "a2a.getTask";
  static constexpr std::string_view kCancelTask = "a2a.cancelTask";
  static constexpr std::string_view kSetTaskPushNotificationConfig =
      "a2a.setTaskPushNotificationConfig";
  static constexpr std::string_view kGetTaskPushNotificationConfig =
      "a2a.getTaskPushNotificationConfig";
  static constexpr std::string_view kListTaskPushNotificationConfigs =
      "a2a.listTaskPushNotificationConfigs";
  static constexpr std::string_view kDeleteTaskPushNotificationConfig =
      "a2a.deleteTaskPushNotificationConfig";
};

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
  const auto header_it = std::ranges::find_if(response.headers, [](const auto& pair) {
    if (pair.first.size() != std::string_view(core::Version::kHeaderName).size()) {
      return false;
    }
    for (std::size_t index = 0; index < pair.first.size(); ++index) {
      if (std::tolower(static_cast<unsigned char>(pair.first[index])) !=
          std::tolower(static_cast<unsigned char>(core::Version::kHeaderName[index]))) {
        return false;
      }
    }
    return true;
  });
  if (header_it == response.headers.end()) {
    return {};
  }

  if (!core::Version::IsSupported(header_it->second)) {
    return core::Error::UnsupportedVersion("Server returned unsupported A2A-Version header")
        .WithTransport("jsonrpc")
        .WithProtocolCode(header_it->second);
  }

  return {};
}

core::Error BuildJsonRpcEnvelopeError(std::string_view message,
                                      const HttpClientResponse& response) {
  return core::Error::RemoteProtocol(std::string(message))
      .WithTransport("jsonrpc")
      .WithHttpStatus(response.status_code);
}

core::Error BuildRemoteJsonRpcError(const google::protobuf::Value& error_value,
                                    const HttpClientResponse& response) {
  if (!error_value.has_struct_value()) {
    return BuildJsonRpcEnvelopeError("JSON-RPC error payload must be an object", response);
  }

  const auto& fields = error_value.struct_value().fields();
  std::string message = "JSON-RPC request failed";
  std::string code;

  const auto message_it = fields.find("message");
  if (message_it != fields.end() && message_it->second.has_string_value()) {
    message = message_it->second.string_value();
  }

  const auto code_it = fields.find("code");
  if (code_it != fields.end()) {
    if (code_it->second.has_number_value()) {
      const auto numeric_code = static_cast<int>(code_it->second.number_value());
      code = std::to_string(numeric_code);
    } else if (code_it->second.has_string_value()) {
      code = code_it->second.string_value();
    }
  }

  core::Error error = core::Error::RemoteProtocol(message).WithTransport("jsonrpc").WithHttpStatus(
      response.status_code);
  if (!code.empty()) {
    error = error.WithProtocolCode(code);
  }
  return error;
}

core::Result<google::protobuf::Struct> ParseEnvelope(std::string_view payload,
                                                     const HttpClientResponse& response) {
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
  if (version_it == fields.end() || !version_it->second.has_string_value() ||
      version_it->second.string_value() != kJsonRpcVersion) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response has invalid version", response);
  }

  const auto id_it = fields.find("id");
  if (id_it == fields.end() || !id_it->second.has_string_value()) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response id must be a string", response);
  }
  if (id_it->second.string_value() != expected_id) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response id does not match request id", response);
  }

  const auto error_it = fields.find("error");
  const auto result_it = fields.find("result");
  if (error_it != fields.end() && result_it != fields.end()) {
    return BuildJsonRpcEnvelopeError("JSON-RPC response must not contain both result and error",
                                     response);
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
core::Result<T> ParseResultMessage(const google::protobuf::Value& result_value,
                                   int response_status_code) {
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

}  // namespace

JsonRpcTransport::JsonRpcTransport(ResolvedInterface resolved_interface, HttpRequester requester,
                                   std::chrono::milliseconds default_timeout,
                                   RequestIdGenerator id_generator)
    : resolved_interface_(std::move(resolved_interface)),
      requester_(std::move(requester)),
      default_timeout_(default_timeout),
      id_generator_(std::move(id_generator)) {
  if (id_generator_ == nullptr) {
    id_generator_ = BuildDefaultRequestId;
  }
}

core::Result<HttpClientResponse> JsonRpcTransport::SendJsonRpcRequest(
    std::string request_body, const CallOptions& options) const {
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
  http_request.method = "POST";
  http_request.url = JoinUrl(resolved_interface_.url);
  http_request.body = std::move(request_body);
  http_request.timeout = options.timeout.value_or(default_timeout_);
  http_request.headers = options.headers;
  http_request.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  http_request.headers["Content-Type"] = "application/json";
  http_request.headers["Accept"] = "application/json";

  if (!options.extensions.empty()) {
    http_request.headers[std::string(core::Extensions::kHeaderName)] =
        core::Extensions::Format(options.extensions);
  }
  if (options.auth_hook) {
    options.auth_hook(http_request.headers);
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

core::Result<google::protobuf::Value> JsonRpcTransport::InvokeForResultValue(
    std::string_view method_name, const google::protobuf::Message& request,
    const CallOptions& options) const {
  if (method_name.empty()) {
    return core::Error::Validation("JSON-RPC method name is required");
  }

  const std::string request_id = id_generator_();
  if (request_id.empty()) {
    return core::Error::Internal("JSON-RPC request id generator returned an empty id");
  }

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
  (*envelope.mutable_fields())["jsonrpc"].set_string_value(std::string(kJsonRpcVersion));
  (*envelope.mutable_fields())["id"].set_string_value(request_id);
  (*envelope.mutable_fields())["method"].set_string_value(std::string(method_name));
  (*envelope.mutable_fields())["params"] = params;

  const auto envelope_json = core::MessageToJson(envelope);
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
  const auto result = InvokeForResultValue(JsonRpcMethodMap::kSendMessage, request, options);
  if (!result.ok()) {
    return result.error();
  }

  const auto response =
      ParseResultMessage<lf::a2a::v1::SendMessageResponse>(result.value(), kHttpOkMin);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::Task> JsonRpcTransport::GetTask(
    const lf::a2a::v1::GetTaskRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskRequest.id is required");
  }

  const auto result = InvokeForResultValue(JsonRpcMethodMap::kGetTask, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response = ParseResultMessage<lf::a2a::v1::Task>(result.value(), kHttpOkMin);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::Task> JsonRpcTransport::CancelTask(
    const lf::a2a::v1::CancelTaskRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("CancelTaskRequest.id is required");
  }

  const auto result = InvokeForResultValue(JsonRpcMethodMap::kCancelTask, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response = ParseResultMessage<lf::a2a::v1::Task>(result.value(), kHttpOkMin);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig>
JsonRpcTransport::SetTaskPushNotificationConfig(
    const lf::a2a::v1::TaskPushNotificationConfig& request, const CallOptions& options) {
  const auto result =
      InvokeForResultValue(JsonRpcMethodMap::kSetTaskPushNotificationConfig, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response =
      ParseResultMessage<lf::a2a::v1::TaskPushNotificationConfig>(result.value(), kHttpOkMin);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::TaskPushNotificationConfig>
JsonRpcTransport::GetTaskPushNotificationConfig(
    const lf::a2a::v1::GetTaskPushNotificationConfigRequest& request, const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("GetTaskPushNotificationConfigRequest.id is required");
  }

  const auto result =
      InvokeForResultValue(JsonRpcMethodMap::kGetTaskPushNotificationConfig, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response =
      ParseResultMessage<lf::a2a::v1::TaskPushNotificationConfig>(result.value(), kHttpOkMin);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>
JsonRpcTransport::ListTaskPushNotificationConfigs(
    const lf::a2a::v1::ListTaskPushNotificationConfigsRequest& request,
    const CallOptions& options) {
  const auto result =
      InvokeForResultValue(JsonRpcMethodMap::kListTaskPushNotificationConfigs, request, options);
  if (!result.ok()) {
    return result.error();
  }
  const auto response = ParseResultMessage<lf::a2a::v1::ListTaskPushNotificationConfigsResponse>(
      result.value(), kHttpOkMin);
  if (!response.ok()) {
    return response.error();
  }
  return response.value();
}

core::Result<void> JsonRpcTransport::DeleteTaskPushNotificationConfig(
    const lf::a2a::v1::DeleteTaskPushNotificationConfigRequest& request,
    const CallOptions& options) {
  if (request.id().empty()) {
    return core::Error::Validation("DeleteTaskPushNotificationConfigRequest.id is required");
  }

  const auto result =
      InvokeForResultValue(JsonRpcMethodMap::kDeleteTaskPushNotificationConfig, request, options);
  if (!result.ok()) {
    return result.error();
  }

  const auto parsed_empty = ParseResultMessage<google::protobuf::Empty>(result.value(), kHttpOkMin);
  if (!parsed_empty.ok()) {
    return parsed_empty.error();
  }
  return {};
}

core::Result<std::unique_ptr<StreamHandle>> JsonRpcTransport::SendStreamingMessage(
    const lf::a2a::v1::SendMessageRequest& request, StreamObserver& observer,
    const CallOptions& options) {
  (void)request;
  (void)observer;
  (void)options;
  return core::Error::Validation("JSON-RPC transport does not support streaming operations");
}

core::Result<std::unique_ptr<StreamHandle>> JsonRpcTransport::SubscribeTask(
    const lf::a2a::v1::GetTaskRequest& request, StreamObserver& observer,
    const CallOptions& options) {
  (void)request;
  (void)observer;
  (void)options;
  return core::Error::Validation("JSON-RPC transport does not support streaming operations");
}

}  // namespace a2a::client
