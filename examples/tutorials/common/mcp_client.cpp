#include "mcp_client.h"

#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "a2a/core/http_utils.h"
#include "a2a/core/protojson.h"
#include "a2a/http/http_client.h"
#include "google/protobuf/struct.pb.h"

namespace tutorial_mcp {
namespace {
constexpr int kOk = 200;
constexpr int kAccepted = 202;
constexpr std::string_view kContentType = "application/json";
constexpr std::string_view kAccept = "application/json, text/event-stream";
constexpr std::string_view kProtocolVersion = "2025-06-18";
constexpr std::string_view kSessionHeader = "Mcp-Session-Id";
constexpr std::string_view kProtocolHeader = "MCP-Protocol-Version";

struct McpResponse final {
  google::protobuf::Struct envelope;
  std::optional<std::string> session_id;
};

std::string Json(const google::protobuf::Message& message) {
  auto json = a2a::core::MessageToJson(message);
  return json.ok() ? std::move(json.value()) : std::string{};
}

std::string JsonString(std::string_view value) {
  google::protobuf::Value json_value;
  json_value.set_string_value(std::string(value));
  return Json(json_value);
}

std::vector<a2a::http::Header> Headers(const std::optional<std::string>& session_id) {
  std::vector<a2a::http::Header> headers = {
      {.name = "Content-Type", .value = std::string(kContentType)},
      {.name = "Accept", .value = std::string(kAccept)},
      {.name = std::string(kProtocolHeader), .value = std::string(kProtocolVersion)},
  };
  if (session_id.has_value()) {
    headers.push_back({.name = std::string(kSessionHeader), .value = *session_id});
  }
  return headers;
}

a2a::core::Result<McpResponse> Post(const std::string& endpoint, std::string body, std::chrono::milliseconds timeout,
                                    const std::optional<std::string>& session_id = std::nullopt) {
  a2a::http::Request request{
      .method = "POST", .url = endpoint, .headers = Headers(session_id), .body = std::move(body), .timeout = timeout};
  auto response = a2a::http::Client{}.SendRequest(request);
  if (!response.ok()) {
    std::string message = "MCP service unavailable: ";
    message.append(response.error().message());
    return a2a::core::Error::Internal(std::move(message));
  }
  if (response.value().status_code != kOk) {
    return a2a::core::Error::Internal("MCP service returned HTTP " + std::to_string(response.value().status_code));
  }
  google::protobuf::Struct envelope;
  auto parsed = a2a::core::JsonToMessage(response.value().body, &envelope);
  if (!parsed.ok()) {
    return a2a::core::Error::Internal("MCP service returned invalid JSON");
  }
  const auto error = envelope.fields().find("error");
  if (error != envelope.fields().end()) {
    const auto message = error->second.struct_value().fields().find("message");
    return a2a::core::Error::Validation(message == error->second.struct_value().fields().end()
                                            ? "MCP resource request failed"
                                            : message->second.string_value());
  }
  const auto session = a2a::core::http::FindHeaderValue(response.value().headers, kSessionHeader);
  return McpResponse{.envelope = std::move(envelope),
                     .session_id = session.has_value() ? std::optional<std::string>(*session) : std::nullopt};
}

a2a::core::Result<void> ValidateInitialization(const McpResponse& response) {
  const auto result = response.envelope.fields().find("result");
  if (result == response.envelope.fields().end()) {
    return a2a::core::Error::Internal("MCP initialization result is missing");
  }
  const auto& fields = result->second.struct_value().fields();
  const auto version = fields.find("protocolVersion");
  if (version == fields.end() || version->second.string_value() != kProtocolVersion) {
    return a2a::core::Error::Validation("MCP server negotiated an unsupported protocol version");
  }
  const auto capabilities = fields.find("capabilities");
  if (capabilities == fields.end() || !capabilities->second.struct_value().fields().contains("resources")) {
    return a2a::core::Error::Validation("MCP server does not advertise resources capability");
  }
  return {};
}

a2a::core::Result<void> NotifyInitialized(const std::string& endpoint, std::chrono::milliseconds timeout,
                                          const std::optional<std::string>& session_id) {
  a2a::http::Request request{.method = "POST",
                             .url = endpoint,
                             .headers = Headers(session_id),
                             .body = R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                             .timeout = timeout};
  auto response = a2a::http::Client{}.SendRequest(request);
  if (!response.ok()) {
    return a2a::core::Error::Internal("MCP initialization notification failed");
  }
  if (response.value().status_code != kAccepted) {
    return a2a::core::Error::Internal("MCP initialization notification was not accepted");
  }
  return {};
}
}  // namespace

Client::Client(std::string endpoint, std::chrono::milliseconds timeout)
    : endpoint_(std::move(endpoint)), timeout_(timeout) {}

a2a::core::Result<std::string> Client::ReadResource(std::string_view uri) const {
  std::ostringstream initialization_payload;
  initialization_payload << R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":)"
                         << JsonString(kProtocolVersion)
                         << R"(,"capabilities":{},"clientInfo":{"name":"a2a-cpp-tutorial","version":"1.0.0"}}})";
  auto initialized = Post(endpoint_, initialization_payload.str(), timeout_);
  if (!initialized.ok()) {
    return initialized.error();
  }
  auto validation = ValidateInitialization(initialized.value());
  if (!validation.ok()) {
    return validation.error();
  }
  auto notified = NotifyInitialized(endpoint_, timeout_, initialized.value().session_id);
  if (!notified.ok()) {
    return notified.error();
  }

  google::protobuf::Value uri_value;
  uri_value.set_string_value(std::string(uri));
  std::ostringstream payload;
  payload << R"({"jsonrpc":"2.0","id":1,"method":"resources/read","params":{"uri":)" << Json(uri_value) << "}}";
  auto response = Post(endpoint_, payload.str(), timeout_, initialized.value().session_id);
  if (!response.ok()) {
    return response.error();
  }
  const auto result = response.value().envelope.fields().find("result");
  if (result == response.value().envelope.fields().end()) {
    return a2a::core::Error::Internal("MCP result is missing");
  }
  const auto contents = result->second.struct_value().fields().find("contents");
  if (contents == result->second.struct_value().fields().end() || contents->second.list_value().values().empty()) {
    return a2a::core::Error::Internal("MCP resource contents are missing");
  }
  const auto& fields = contents->second.list_value().values(0).struct_value().fields();
  const auto text = fields.find("text");
  if (text == fields.end() || text->second.string_value().empty()) {
    return a2a::core::Error::Internal("MCP resource text is missing");
  }
  return text->second.string_value();
}
}  // namespace tutorial_mcp
