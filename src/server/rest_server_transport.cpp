#include "a2a/server/rest_server_transport.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/legacy_transport_names.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::server {
namespace {

constexpr int kHttpBadRequest = 400;
constexpr int kHttpNotFound = 404;
constexpr int kHttpOk = 200;
constexpr int kHexAlphabetOffset = 10;

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
  for (const auto& [header_name, value] : headers) {
    if (ToLower(header_name) == lowered_name) {
      return value;
    }
  }
  return {};
}

std::string HttpStatusName(int status_code) {
  switch (status_code) {
    case kHttpBadRequest:
      return "INVALID_ARGUMENT";
    case kHttpNotFound:
      return "NOT_FOUND";
    default:
      return "UNKNOWN";
  }
}

std::string ErrorBody(int status_code, std::string_view message, std::string_view reason) {
  google::protobuf::Struct error_info;
  auto* error_info_fields = error_info.mutable_fields();
  (*error_info_fields)["@type"].set_string_value("type.googleapis.com/google.rpc.ErrorInfo");
  (*error_info_fields)["reason"].set_string_value(std::string(reason));
  (*error_info_fields)["domain"].set_string_value("a2a-protocol.org");

  google::protobuf::Struct envelope;
  auto* envelope_fields = envelope.mutable_fields();
  google::protobuf::Value error_value;
  auto* error_fields = error_value.mutable_struct_value()->mutable_fields();

  (*error_fields)["code"].set_number_value(status_code);
  (*error_fields)["status"].set_string_value(HttpStatusName(status_code));
  (*error_fields)["message"].set_string_value(std::string(message));

  google::protobuf::Value details;
  auto* details_values = details.mutable_list_value()->mutable_values();
  google::protobuf::Value error_info_value;
  *error_info_value.mutable_struct_value() = std::move(error_info);
  details_values->Add(std::move(error_info_value));
  (*error_fields)["details"] = std::move(details);

  (*envelope_fields)["error"] = std::move(error_value);

  const auto serialized = core::MessageToJson(envelope);
  if (serialized.ok()) {
    return serialized.value();
  }
  return R"({"error":{"code":500,"status":"INTERNAL","message":"serialization failed"}})";
}

HttpServerResponse BuildJsonErrorResponse(int status_code, std::string_view message,
                                          std::string_view reason) {
  HttpServerResponse response;
  response.status_code = status_code;
  response.headers["Content-Type"] = "application/json";
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  response.body = ErrorBody(status_code, message, reason);
  return response;
}

google::protobuf::Value* EnsureStructField(google::protobuf::Struct* object, std::string key) {
  auto& value = (*object->mutable_fields())[std::move(key)];
  if (!value.has_struct_value()) {
    value.mutable_struct_value();
  }
  return &value;
}

google::protobuf::Value* EnsureListField(google::protobuf::Struct* object, std::string key) {
  auto& value = (*object->mutable_fields())[std::move(key)];
  if (!value.has_list_value()) {
    value.mutable_list_value();
  }
  return &value;
}

core::Result<std::string> DecodeUrlComponent(std::string_view raw) {
  std::string decoded;
  decoded.reserve(raw.size());

  for (std::size_t index = 0; index < raw.size(); ++index) {
    const char ch = raw[index];
    if (ch == '+') {
      decoded.push_back(' ');
      continue;
    }
    if (ch != '%') {
      decoded.push_back(ch);
      continue;
    }

    if (index + 2 >= raw.size()) {
      return core::Error::Validation("Malformed URL encoding");
    }

    const auto hex_to_int = [](char hex) -> int {
      if (hex >= '0' && hex <= '9') {
        return hex - '0';
      }
      if (hex >= 'a' && hex <= 'f') {
        return kHexAlphabetOffset + (hex - 'a');
      }
      if (hex >= 'A' && hex <= 'F') {
        return kHexAlphabetOffset + (hex - 'A');
      }
      return -1;
    };

    const int hi = hex_to_int(raw[index + 1]);
    const int lo = hex_to_int(raw[index + 2]);
    if (hi < 0 || lo < 0) {
      return core::Error::Validation("Malformed URL encoding");
    }
    decoded.push_back(static_cast<char>((hi << 4) | lo));
    index += 2;
  }

  return decoded;
}

core::Result<void> ParseQueryString(std::string_view raw,
                                    std::unordered_map<std::string, std::string>* out) {
  if (out == nullptr) {
    return core::Error::Internal("Query output map is required");
  }
  out->clear();
  if (raw.empty()) {
    return {};
  }

  std::size_t start = 0;
  while (start <= raw.size()) {
    const std::size_t end = raw.find('&', start);
    const auto part =
        raw.substr(start, end == std::string_view::npos ? raw.size() - start : end - start);
    if (!part.empty()) {
      const std::size_t split = part.find('=');
      const auto key_raw = part.substr(0, split);
      const auto value_raw =
          split == std::string_view::npos ? std::string_view{} : part.substr(split + 1);

      const auto key = DecodeUrlComponent(key_raw);
      if (!key.ok()) {
        return key.error();
      }
      const auto value = DecodeUrlComponent(value_raw);
      if (!value.ok()) {
        return value.error();
      }
      out->insert_or_assign(key.value(), value.value());
    }

    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1;
  }

  return {};
}

void AddLegacyTransportFields(google::protobuf::Struct* card,
                              const lf::a2a::v1::AgentCard& agent_card) {
  if (card == nullptr) {
    return;
  }

  auto* fields = card->mutable_fields();
  auto interfaces_it = fields->find("supportedInterfaces");
  if (interfaces_it != fields->end() && interfaces_it->second.has_list_value()) {
    for (auto& interface_value : *interfaces_it->second.mutable_list_value()->mutable_values()) {
      if (!interface_value.has_struct_value()) {
        continue;
      }
      auto* interface_fields = interface_value.mutable_struct_value()->mutable_fields();
      const auto binding_it = interface_fields->find("protocolBinding");
      if (binding_it == interface_fields->end() || !binding_it->second.has_string_value()) {
        continue;
      }
      (*interface_fields)[std::string(a2a::core::legacy_transport_names::kTransportField)]
          .set_string_value(binding_it->second.string_value());
    }
  }

  if (fields->find(std::string(a2a::core::legacy_transport_names::kEndpointField)) == fields->end()) {
    for (const auto& iface : agent_card.supported_interfaces()) {
      if (iface.protocol_binding() == a2a::core::protocol_bindings::kJsonRpc ||
          iface.protocol_binding() == a2a::core::protocol_bindings::kHttpJson) {
        (*fields)[std::string(a2a::core::legacy_transport_names::kEndpointField)]
            .set_string_value(iface.url());
        (*fields)[std::string(a2a::core::legacy_transport_names::kPreferredTransportField)]
            .set_string_value(iface.protocol_binding());
        break;
      }
    }
  }
}

}  // namespace

RestServerTransport::RestServerTransport(Dispatcher* dispatcher, lf::a2a::v1::AgentCard agent_card,
                                         RestServerTransportOptions options)
    : transport_(dispatcher), agent_card_(std::move(agent_card)), options_(std::move(options)) {
  options_.rest_api_base_path = NormalizeBasePath(options_.rest_api_base_path);
  for (auto& iface : *agent_card_.mutable_supported_interfaces()) {
    if (iface.protocol_version().empty()) {
      iface.set_protocol_version(core::Version::HeaderValue());
    }
  }
}

core::Result<HttpServerResponse> RestServerTransport::Handle(
    const HttpServerRequest& request) const {
  if (request.target.empty() || request.target.front() != '/') {
    return core::Error::Validation("HTTP request target must start with '/'");
  }

  const auto query_start = request.target.find('?');
  const std::string_view path = query_start == std::string::npos
                                    ? std::string_view(request.target)
                                    : std::string_view(request.target).substr(0, query_start);

  if (path == kAgentCardPath || path == kLegacyAgentCardPath) {
    return HandleAgentCard(request);
  }

  const auto version = ValidateVersionHeader(request);
  if (!version.ok()) {
    return BuildJsonErrorResponse(kHttpBadRequest, version.error().message(),
                                  "VERSION_NOT_SUPPORTED");
  }

  const auto rest_request = BuildRestRequest(request);
  if (!rest_request.ok()) {
    return BuildJsonErrorResponse(kHttpNotFound, "No matching route or request was malformed",
                                  "UNSUPPORTED_OPERATION");
  }

  const auto rest_response = transport_.Handle(rest_request.value());
  if (!rest_response.ok()) {
    return rest_response.error();
  }
  return ToHttpResponse(rest_response.value());
}

core::Result<RestRequest> RestServerTransport::BuildRestRequest(
    const HttpServerRequest& request) const {
  const auto query_start = request.target.find('?');
  std::string path =
      query_start == std::string::npos ? request.target : request.target.substr(0, query_start);

  if (!options_.rest_api_base_path.empty() && options_.rest_api_base_path != "/") {
    if (!path.starts_with(options_.rest_api_base_path)) {
      return core::Error::Validation("Request path does not match configured REST API base path");
    }
    path = path.substr(options_.rest_api_base_path.size());
    if (path.empty()) {
      path = "/";
    }
  }

  RestRequest rest_request;
  rest_request.method = request.method;
  rest_request.path = std::move(path);
  rest_request.body = request.body;
  rest_request.context.remote_address = request.remote_address.empty()
                                            ? std::optional<std::string>{}
                                            : std::optional<std::string>(request.remote_address);
  rest_request.context.client_headers = request.headers;
  rest_request.context.auth_metadata = ExtractAuthMetadata(request.headers);

  if (query_start != std::string::npos) {
    const auto parsed = ParseQueryString(std::string_view(request.target).substr(query_start + 1),
                                         &rest_request.query_params);
    if (!parsed.ok()) {
      return parsed.error();
    }
  }

  return rest_request;
}

core::Result<void> RestServerTransport::ValidateVersionHeader(
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

core::Result<HttpServerResponse> RestServerTransport::HandleAgentCard(
    const HttpServerRequest& request) const {
  if (request.method != "GET") {
    return BuildJsonErrorResponse(kHttpNotFound, "No matching route or request was malformed",
                                  "UNSUPPORTED_OPERATION");
  }

  const auto body = core::MessageToJson(agent_card_);
  if (!body.ok()) {
    return body.error();
  }

  google::protobuf::Struct card;
  const auto parsed = core::JsonToMessage(body.value(), &card, {.ignore_unknown_fields = false});
  if (!parsed.ok()) {
    return parsed.error();
  }
  auto* fields = card.mutable_fields();
  if (fields->find("version") == fields->end()) {
    (*fields)["version"].set_string_value("0.1.0");
  }
  if (fields->find("description") == fields->end()) {
    (*fields)["description"].set_string_value("");
  }
  auto* capabilities = EnsureStructField(&card, "capabilities")->mutable_struct_value();
  auto* cap_fields = capabilities->mutable_fields();
  if (cap_fields->find("streaming") == cap_fields->end()) {
    (*cap_fields)["streaming"].set_bool_value(false);
  }
  if (cap_fields->find("pushNotifications") == cap_fields->end()) {
    (*cap_fields)["pushNotifications"].set_bool_value(false);
  }
  if (fields->find("defaultInputModes") == fields->end()) {
    auto* modes = EnsureListField(&card, "defaultInputModes")->mutable_list_value();
    modes->add_values()->set_string_value("text/plain");
  }
  if (fields->find("defaultOutputModes") == fields->end()) {
    auto* modes = EnsureListField(&card, "defaultOutputModes")->mutable_list_value();
    modes->add_values()->set_string_value("text/plain");
  }
  if (fields->find("skills") == fields->end()) {
    EnsureListField(&card, "skills");
  }
  auto skills_it = fields->find("skills");
  if (skills_it != fields->end() && skills_it->second.has_list_value()) {
    for (auto& skill : *skills_it->second.mutable_list_value()->mutable_values()) {
      if (!skill.has_struct_value()) continue;
      EnsureListField(skill.mutable_struct_value(), "tags");
    }
  }

  if (options_.include_legacy_transport_fields) {
    AddLegacyTransportFields(&card, agent_card_);
  }

  const auto normalized = core::MessageToJson(card);
  if (!normalized.ok()) {
    return normalized.error();
  }

  HttpServerResponse response;
  response.status_code = kHttpOk;
  response.headers["Content-Type"] = "application/json";
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  response.body = normalized.value();
  return response;
}

HttpServerResponse RestServerTransport::ToHttpResponse(const RestResponse& response) {
  HttpServerResponse http_response;
  http_response.status_code = response.http_status;
  http_response.headers = response.headers;
  http_response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  http_response.body = response.body;
  return http_response;
}

std::string RestServerTransport::NormalizeBasePath(std::string_view path) {
  std::string normalized(path);
  if (normalized.empty()) {
    return "/";
  }
  if (normalized.front() != '/') {
    normalized.insert(normalized.begin(), '/');
  }
  while (normalized.size() > 1 && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized;
}

}  // namespace a2a::server
