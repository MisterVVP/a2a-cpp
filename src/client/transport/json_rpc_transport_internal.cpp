// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "json_rpc_transport_internal.h"

#include <google/protobuf/struct.pb.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/http_utils.h"
#include "a2a/core/json_rpc.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::client::json_rpc_internal {
namespace {

constexpr std::size_t kJsonRpcRequestEnvelopeOverhead = 45U;
constexpr char kJsonRpcTransportName[] = "jsonrpc";
constexpr std::string_view kUnsupportedVersionMessage = "Server returned unsupported A2A-Version header";
constexpr std::string_view kInvalidErrorPayloadMessage = "JSON-RPC error payload must be an object";
constexpr std::string_view kRequestFailedMessage = "JSON-RPC request failed";
constexpr std::string_view kMessageMemberName = "message";
constexpr std::string_view kCodeMemberName = "code";
constexpr std::string_view kErrorMemberName = "error";
constexpr std::string_view kInvalidVersionMessage = "JSON-RPC response has invalid version";
constexpr std::string_view kInvalidIdMessage = "JSON-RPC response id must be a string";
constexpr std::string_view kMismatchedIdMessage = "JSON-RPC response id does not match request id";
constexpr std::string_view kResultAndErrorMessage = "JSON-RPC response must not contain both result and error";
constexpr std::string_view kMissingResultMessage = "JSON-RPC response is missing result";
constexpr std::string_view kEnvelopeSerializationMessage = "Failed to serialize JSON-RPC envelope fields";

}  // namespace

a2a::http::Request ToSharedHttpRequest(const HttpRequest& request) {
  a2a::http::Request converted{
      .method = request.method, .url = request.url, .headers = {}, .body = request.body, .timeout = request.timeout};
  converted.headers.reserve(request.headers.size());
  for (const auto& [name, value] : request.headers) {
    converted.headers.push_back({.name = name, .value = value});
  }
  return converted;
}

HttpClientResponse ToClientHttpResponse(a2a::http::Response response) {
  HttpClientResponse converted{.status_code = response.status_code, .headers = {}, .body = std::move(response.body)};
  for (auto& header : response.headers) {
    converted.headers[std::move(header.name)] = std::move(header.value);
  }
  return converted;
}

std::string JoinUrl(std::string_view interface_base_url) {
  std::string base(interface_base_url);
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base;
}

core::Result<void> ValidateResponseVersion(const HttpClientResponse& response) {
  const auto version = core::http::FindHeaderValue(response.headers, core::Version::kHeaderName);
  if (!version.has_value()) {
    return {};
  }

  if (!core::Version::IsSupported(version.value())) {
    return core::Error::UnsupportedVersion(std::string(kUnsupportedVersionMessage))
        .WithTransport(kJsonRpcTransportName)
        .WithProtocolCode(std::string(version.value()));
  }

  return {};
}

core::Error BuildJsonRpcEnvelopeError(std::string_view message, const HttpClientResponse& response) {
  return core::Error::RemoteProtocol(std::string(message))
      .WithTransport(kJsonRpcTransportName)
      .WithHttpStatus(response.status_code);
}

core::Error BuildRemoteJsonRpcError(const google::protobuf::Value& error_value, const HttpClientResponse& response) {
  if (!error_value.has_struct_value()) {
    return BuildJsonRpcEnvelopeError(kInvalidErrorPayloadMessage, response);
  }

  const auto& fields = error_value.struct_value().fields();
  std::string message(kRequestFailedMessage);
  std::string code;

  const auto message_it = fields.find(kMessageMemberName);
  if (message_it != fields.end() && message_it->second.kind_case() == ::google::protobuf::Value::kStringValue) {
    message = message_it->second.string_value();
  }

  const auto code_it = fields.find(kCodeMemberName);
  if (code_it != fields.end()) {
    if (code_it->second.kind_case() == ::google::protobuf::Value::kNumberValue) {
      const auto numeric_code = static_cast<int>(code_it->second.number_value());
      code = std::to_string(numeric_code);
    } else if (code_it->second.kind_case() == ::google::protobuf::Value::kStringValue) {
      code = code_it->second.string_value();
    }
  }

  core::Error error =
      core::Error::RemoteProtocol(message).WithTransport(kJsonRpcTransportName).WithHttpStatus(response.status_code);
  if (!code.empty()) {
    error = error.WithProtocolCode(code);
  }
  return error;
}

core::Result<google::protobuf::Struct> ParseEnvelope(std::string_view payload, const HttpClientResponse& response) {
  google::protobuf::Struct envelope;
  const auto parse = core::JsonToMessage(payload, &envelope);
  if (!parse.ok()) {
    return parse.error().WithTransport(kJsonRpcTransportName).WithHttpStatus(response.status_code);
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
  const auto version_it = fields.find(core::json_rpc::kVersionMemberName);
  if (version_it == fields.end() || version_it->second.kind_case() != ::google::protobuf::Value::kStringValue ||
      version_it->second.string_value() != core::json_rpc::kVersion) {
    return BuildJsonRpcEnvelopeError(kInvalidVersionMessage, response);
  }

  const auto id_it = fields.find(core::json_rpc::kIdMemberName);
  if (id_it == fields.end() || id_it->second.kind_case() != ::google::protobuf::Value::kStringValue) {
    return BuildJsonRpcEnvelopeError(kInvalidIdMessage, response);
  }
  if (id_it->second.string_value() != expected_id) {
    return BuildJsonRpcEnvelopeError(kMismatchedIdMessage, response);
  }

  const auto error_it = fields.find(kErrorMemberName);
  const auto result_it = fields.find(core::json_rpc::kResultMemberName);
  if (error_it != fields.end() && result_it != fields.end()) {
    return BuildJsonRpcEnvelopeError(kResultAndErrorMessage, response);
  }

  if (error_it != fields.end()) {
    return BuildRemoteJsonRpcError(error_it->second, response);
  }

  if (result_it == fields.end()) {
    return BuildJsonRpcEnvelopeError(kMissingResultMessage, response);
  }

  return result_it->second;
}

// Shared by unary and SSE JSON-RPC calls so request envelope fields stay consistent.
core::Result<std::string> BuildJsonRpcEnvelope(std::string_view method_name, const google::protobuf::Message& request,
                                               std::string_view request_id) {
  const auto request_json = core::MessageToJson(request);
  if (!request_json.ok()) {
    return request_json.error();
  }
  google::protobuf::Value id;
  id.set_string_value(std::string(request_id));
  google::protobuf::Value method;
  method.set_string_value(std::string(method_name));
  const auto id_json = core::MessageToJson(id);
  const auto method_json = core::MessageToJson(method);
  if (!id_json.ok() || !method_json.ok()) {
    return core::Error::Serialization(std::string(kEnvelopeSerializationMessage));
  }
  std::string envelope;
  envelope.reserve(request_json.value().size() + id_json.value().size() + method_json.value().size() +
                   kJsonRpcRequestEnvelopeOverhead);
  envelope.push_back('{');
  envelope.push_back('"');
  envelope.append(core::json_rpc::kVersionMemberName);
  envelope.push_back('"');
  envelope.push_back(':');
  envelope.push_back('"');
  envelope.append(core::json_rpc::kVersion);
  envelope.push_back('"');
  envelope.push_back(',');
  envelope.push_back('"');
  envelope.append(core::json_rpc::kIdMemberName);
  envelope.push_back('"');
  envelope.push_back(':');
  envelope.append(id_json.value());
  envelope.push_back(',');
  envelope.push_back('"');
  envelope.append(core::json_rpc::kMethodMemberName);
  envelope.push_back('"');
  envelope.push_back(':');
  envelope.append(method_json.value());
  envelope.push_back(',');
  envelope.push_back('"');
  envelope.append(core::json_rpc::kParamsMemberName);
  envelope.push_back('"');
  envelope.push_back(':');
  envelope.append(request_json.value());
  envelope.push_back('}');
  return envelope;
}

}  // namespace a2a::client::json_rpc_internal
