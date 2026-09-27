#include "mcp_client.h"

#include <sstream>
#include <utility>
#include <vector>

#include "a2a/core/protojson.h"
#include "a2a/http/http_client.h"
#include "google/protobuf/struct.pb.h"

namespace tutorial_mcp {
namespace {
constexpr int kRequestId = 1;
constexpr int kMinimumSuccessStatus = 200;
constexpr int kMaximumSuccessStatus = 299;
constexpr std::string_view kJsonRpcVersion = "2.0";
constexpr std::string_view kProtocolVersion = "2026-07-28";
constexpr std::string_view kMethod = "resources/read";

std::string JsonString(std::string_view value) {
  google::protobuf::Value json_value;
  json_value.set_string_value(std::string(value));
  auto json = a2a::core::MessageToJson(json_value);
  return json.ok() ? std::move(json.value()) : std::string{};
}

std::string RequestBody(std::string_view uri) {
  std::ostringstream body;
  body << R"({"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":)" << JsonString(uri)
       << R"(,"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
          R"("io.modelcontextprotocol/clientInfo":{"name":"a2a-cpp-tutorial","version":"1.0.0"},)"
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
  google::protobuf::Struct envelope;
  if (!a2a::core::JsonToMessage(response.value().body, &envelope).ok()) {
    return a2a::core::Error::Validation("MCP service returned malformed JSON");
  }
  auto result = ValidateEnvelope(envelope);
  if (!result.ok()) {
    return result.error();
  }
  return ReadText(*result.value(), uri);
}
}  // namespace tutorial_mcp
