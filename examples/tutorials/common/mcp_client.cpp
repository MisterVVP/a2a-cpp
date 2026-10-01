#include "mcp_client.h"

#include <cmath>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "a2a/client/sse_parser.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/protojson.h"
#include "a2a/http/http_client.h"
#include "google/protobuf/struct.pb.h"
#include "resource_validation.h"

namespace tutorial_mcp {
namespace {
constexpr int kRequestId = 1;
constexpr int kMinimumSuccessStatus = 200;
constexpr int kMaximumSuccessStatus = 299;
constexpr std::string_view kJsonRpcVersion = "2.0";
constexpr std::string_view kProtocolVersion = "2025-11-25";
constexpr std::string_view kMethod = "resources/read";
constexpr std::string_view kResourceUriLabel = "MCP resource URI";

std::string JsonString(std::string_view value) {
  google::protobuf::Value json_value;
  json_value.set_string_value(std::string(value));
  auto json = a2a::core::MessageToJson(json_value);
  return json.ok() ? std::move(json.value()) : std::string{};
}

std::string RequestBody(std::string_view uri) {
  std::ostringstream body;
  body << R"({"jsonrpc":")" << kJsonRpcVersion << R"(","id":)" << kRequestId << R"(,"method":")" << kMethod
       << R"(","params":{"uri":)" << JsonString(uri) << R"(,"_meta":{"io.modelcontextprotocol/protocolVersion":")"
       << kProtocolVersion
       << R"(","io.modelcontextprotocol/clientInfo":{"name":"a2a-cpp-tutorial","version":"1.0.0"},)"
          R"("io.modelcontextprotocol/clientCapabilities":{}}}})";
  return body.str();
}

std::vector<a2a::http::Header> RequestHeaders(std::string_view uri, std::string_view bearer_token) {
  std::string authorization = "Bearer ";
  authorization.append(bearer_token);
  return {{.name = "Content-Type", .value = "application/json"},
          {.name = "Accept", .value = "application/json, text/event-stream"},
          {.name = "MCP-Protocol-Version", .value = std::string(kProtocolVersion)},
          {.name = "Mcp-Method", .value = std::string(kMethod)},
          {.name = "Mcp-Name", .value = std::string(uri)},
          {.name = "Authorization", .value = std::move(authorization)}};
}

a2a::core::Result<google::protobuf::Struct> ParseJsonEnvelope(std::string_view body) {
  google::protobuf::Struct envelope;
  if (!a2a::core::JsonToMessage(body, &envelope).ok()) {
    return a2a::core::Error::Validation("MCP service returned malformed JSON");
  }
  return envelope;
}

a2a::core::Result<google::protobuf::Struct> ParseSseEnvelope(std::string_view body) {
  std::optional<google::protobuf::Struct> envelope;
  a2a::client::SseParser parser;
  const auto capture = [&envelope](const a2a::client::SseEvent& event) -> a2a::core::Result<void> {
    if (event.data.empty()) {
      return a2a::core::Error::Validation("MCP SSE response data is missing");
    }
    if (envelope.has_value()) {
      return a2a::core::Error::Validation("MCP service returned multiple SSE messages");
    }
    auto parsed = ParseJsonEnvelope(event.data);
    if (!parsed.ok()) {
      return parsed.error();
    }
    envelope = std::move(parsed.value());
    return {};
  };
  auto parsed = parser.Feed(body, capture);
  if (!parsed.ok()) {
    return parsed.error();
  }
  parsed = parser.Finish(capture);
  if (!parsed.ok()) {
    return parsed.error();
  }
  if (!envelope.has_value()) {
    return a2a::core::Error::Validation("MCP service returned an empty SSE response");
  }
  return std::move(*envelope);
}

a2a::core::Result<google::protobuf::Struct> ParseResponseEnvelope(const a2a::http::Response& response) {
  const auto content_type = a2a::core::http::FindHeaderValue(response.headers, a2a::core::http::kContentTypeHeaderName);
  if (content_type.has_value() && a2a::core::http::IsSseContentType(*content_type)) {
    return ParseSseEnvelope(response.body);
  }
  return ParseJsonEnvelope(response.body);
}

std::optional<a2a::core::Error> JsonRpcError(const google::protobuf::Struct& envelope) {
  const auto error = envelope.fields().find("error");
  if (error == envelope.fields().end()) {
    return std::nullopt;
  }
  if (error->second.kind_case() != google::protobuf::Value::kStructValue) {
    return a2a::core::Error::Validation("MCP JSON-RPC error object is malformed");
  }
  const auto& error_fields = error->second.struct_value().fields();
  const auto code = error_fields.find("code");
  if (code == error_fields.end() || code->second.kind_case() != google::protobuf::Value::kNumberValue) {
    return a2a::core::Error::Validation("MCP JSON-RPC error object is malformed");
  }
  const double code_value = code->second.number_value();
  if (!std::isfinite(code_value) || std::trunc(code_value) != code_value) {
    return a2a::core::Error::Validation("MCP JSON-RPC error object is malformed");
  }
  const auto message = error_fields.find("message");
  if (message == error_fields.end() || message->second.kind_case() != google::protobuf::Value::kStringValue) {
    return a2a::core::Error::Validation("MCP JSON-RPC error object is malformed");
  }
  std::ostringstream protocol_code;
  protocol_code << code_value;
  return a2a::core::Error::RemoteProtocol(message->second.string_value()).WithProtocolCode(protocol_code.str());
}

a2a::core::Result<const google::protobuf::Struct*> ValidateEnvelope(const google::protobuf::Struct& envelope) {
  const auto version = envelope.fields().find("jsonrpc");
  if (version == envelope.fields().end() || version->second.kind_case() != google::protobuf::Value::kStringValue ||
      version->second.string_value() != kJsonRpcVersion) {
    return a2a::core::Error::Validation("MCP response has an invalid JSON-RPC version");
  }
  const auto id = envelope.fields().find("id");
  if (id == envelope.fields().end() || id->second.kind_case() != google::protobuf::Value::kNumberValue ||
      id->second.number_value() != static_cast<double>(kRequestId)) {
    return a2a::core::Error::Validation("MCP response ID does not match the request");
  }
  auto response_error = JsonRpcError(envelope);
  if (response_error.has_value()) {
    return std::move(*response_error);
  }
  const auto result = envelope.fields().find("result");
  if (result == envelope.fields().end() || result->second.kind_case() != google::protobuf::Value::kStructValue) {
    return a2a::core::Error::Validation("MCP response result is missing");
  }
  return &result->second.struct_value();
}

a2a::core::Result<std::string> ReadText(const google::protobuf::Struct& result, std::string_view requested_uri) {
  const auto contents = result.fields().find("contents");
  if (contents == result.fields().end() || contents->second.kind_case() != google::protobuf::Value::kListValue ||
      contents->second.list_value().values().empty()) {
    return a2a::core::Error::Validation("MCP resource contents are missing");
  }
  for (const auto& content : contents->second.list_value().values()) {
    if (content.kind_case() != google::protobuf::Value::kStructValue) {
      continue;
    }
    const auto& fields = content.struct_value().fields();
    const auto uri = fields.find("uri");
    const auto text = fields.find("text");
    if (uri == fields.end() || uri->second.kind_case() != google::protobuf::Value::kStringValue ||
        uri->second.string_value() != requested_uri) {
      return a2a::core::Error::Validation("MCP resource content URI does not match the request");
    }
    if (text != fields.end() && text->second.kind_case() == google::protobuf::Value::kStringValue &&
        !text->second.string_value().empty()) {
      return text->second.string_value();
    }
  }
  return a2a::core::Error::Validation("MCP resource contains no non-empty text");
}
}  // namespace

Client::Client(std::string endpoint, std::string bearer_token, std::chrono::milliseconds timeout)
    : endpoint_(std::move(endpoint)), bearer_token_(std::move(bearer_token)), timeout_(timeout) {}

a2a::core::Result<std::string> Client::ReadResource(std::string_view uri) const {
  if (endpoint_.empty()) {
    return a2a::core::Error::Validation("MCP URL is required for resource mode");
  }
  if (bearer_token_.empty()) {
    return a2a::core::Error::Validation("MCP token is required for resource mode");
  }
  auto uri_validation = ValidateResourceUri(uri, kResourceUriLabel);
  if (!uri_validation.ok()) {
    return uri_validation.error();
  }
  const a2a::http::Request request{.method = "POST",
                                   .url = endpoint_,
                                   .headers = RequestHeaders(uri, bearer_token_),
                                   .body = RequestBody(uri),
                                   .timeout = timeout_};
  auto response = a2a::http::Client{}.SendRequest(request);
  if (!response.ok()) {
    return a2a::core::Error::Internal("MCP service is unavailable");
  }
  if (response.value().status_code < kMinimumSuccessStatus || response.value().status_code > kMaximumSuccessStatus) {
    std::string message = "MCP service returned HTTP ";
    message.append(std::to_string(response.value().status_code));
    return a2a::core::Error::Internal(std::move(message));
  }
  auto envelope = ParseResponseEnvelope(response.value());
  if (!envelope.ok()) {
    return envelope.error();
  }
  auto result = ValidateEnvelope(envelope.value());
  if (!result.ok()) {
    return result.error();
  }
  return ReadText(*result.value(), uri);
}
}  // namespace tutorial_mcp
