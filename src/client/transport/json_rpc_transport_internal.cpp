// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "json_rpc_transport_internal.h"

#include <google/protobuf/struct.pb.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/http_utils.h"
#include "a2a/core/json_rpc.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "core/json.h"

namespace a2a::client::json_rpc_internal {
namespace {

constexpr std::size_t kJsonRpcRequestEnvelopeOverhead = 45U;
constexpr const char* kJsonRpcTransportName = "jsonrpc";
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
  a2a::http::Request converted{.method = request.method,
                               .url = request.url,
                               .headers = {},
                               .body = request.body,
                               .timeout = request.timeout,
                               .tls = request.tls};
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

core::Error BuildRemoteJsonRpcError(simdjson::dom::element error_value, const HttpClientResponse& response) {
  if (!error_value.is_object()) {
    return BuildJsonRpcEnvelopeError(kInvalidErrorPayloadMessage, response);
  }
  std::string message(kRequestFailedMessage);
  std::string code;
  std::string_view text;
  if (error_value[kMessageMemberName].get_string().get(text) == simdjson::SUCCESS) {
    message = text;
  }
  simdjson::dom::element code_value;
  if (error_value[kCodeMemberName].get(code_value) == simdjson::SUCCESS) {
    if (code_value.is_string()) {
      code = std::string_view(code_value.get_string());
    } else if (code_value.is_number()) {
      // Preserve legacy code normalization without narrowing out-of-range input.
      const double number = code_value.get_double();
      if (number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max()) {
        code = std::to_string(static_cast<int>(number));
      } else {
        code = simdjson::minify(code_value);
      }
    }
  }
  auto error =
      core::Error::RemoteProtocol(message).WithTransport(kJsonRpcTransportName).WithHttpStatus(response.status_code);
  return code.empty() ? error : error.WithProtocolCode(code);
}

core::Result<std::string> ParseResponseResult(const HttpClientResponse& response, std::string_view expected_id) {
  core::internal::JsonDocument document;
  const auto parsed = document.ParseObject(response.body);
  if (!parsed.ok()) {
    return parsed.error().WithTransport(kJsonRpcTransportName).WithHttpStatus(response.status_code);
  }
  const auto envelope = document.root();
  std::string_view text;
  if (envelope[core::json_rpc::kVersionMemberName].get_string().get(text) != simdjson::SUCCESS ||
      text != core::json_rpc::kVersion) {
    return BuildJsonRpcEnvelopeError(kInvalidVersionMessage, response);
  }
  if (envelope[core::json_rpc::kIdMemberName].get_string().get(text) != simdjson::SUCCESS) {
    return BuildJsonRpcEnvelopeError(kInvalidIdMessage, response);
  }
  if (text != expected_id) {
    return BuildJsonRpcEnvelopeError(kMismatchedIdMessage, response);
  }
  simdjson::dom::element error;
  simdjson::dom::element result;
  const bool has_error = envelope[kErrorMemberName].get(error) == simdjson::SUCCESS;
  const bool has_result = envelope[core::json_rpc::kResultMemberName].get(result) == simdjson::SUCCESS;
  if (has_error && has_result) {
    return BuildJsonRpcEnvelopeError(kResultAndErrorMessage, response);
  }
  if (has_error) {
    return BuildRemoteJsonRpcError(error, response);
  }
  if (!has_result) {
    return BuildJsonRpcEnvelopeError(kMissingResultMessage, response);
  }
  return core::internal::JsonMember(response.body, core::json_rpc::kResultMemberName);
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
