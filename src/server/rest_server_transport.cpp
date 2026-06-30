// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/rest_server_transport.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "a2a/core/error.h"
#include "a2a/core/extensions.h"
#include "a2a/core/http_constants.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/protocol_codes.h"
#include "a2a/core/protocol_errors.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "a2a/server/agent_card/agent_card_serializer.h"

namespace a2a::server {
namespace {

constexpr int kHexAlphabetOffset = 10;
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::string_view kAgentCardViewQueryKey = "view";
constexpr std::string_view kExtendedAgentCardViewQueryValue = "extended";

struct ErrorBodySpec final {
  int status_code = core::http::kStatusBadRequest;
  std::string_view message;
  std::string_view reason;
};

std::string HttpStatusName(int status_code) {
  switch (status_code) {
    case core::http::kStatusBadRequest:
      return "INVALID_ARGUMENT";
    case core::http::kStatusNotFound:
      return "NOT_FOUND";
    case core::http::kStatusUnauthorized:
      return "UNAUTHENTICATED";
    case core::http::kStatusForbidden:
      return "PERMISSION_DENIED";
    case core::http::kStatusConflict:
      return "CONFLICT";
    case core::http::kStatusUnsupportedMediaType:
      return "UNSUPPORTED_MEDIA_TYPE";
    case core::http::kStatusInternalServerError:
      return "INTERNAL";
    case core::http::kStatusBadGateway:
      return "BAD_GATEWAY";
    default:
      return "UNKNOWN";
  }
}

std::string ErrorBody(const ErrorBodySpec& spec) {
  google::protobuf::Struct error_info;
  auto* error_info_fields = error_info.mutable_fields();
  (*error_info_fields)["@type"].set_string_value("type.googleapis.com/google.rpc.ErrorInfo");
  (*error_info_fields)["reason"].set_string_value(std::string(spec.reason));
  (*error_info_fields)["domain"].set_string_value("a2a-protocol.org");

  google::protobuf::Struct envelope;
  auto* envelope_fields = envelope.mutable_fields();
  google::protobuf::Value error_value;
  auto* error_fields = error_value.mutable_struct_value()->mutable_fields();

  (*error_fields)["code"].set_number_value(spec.status_code);
  (*error_fields)["status"].set_string_value(HttpStatusName(spec.status_code));
  (*error_fields)["message"].set_string_value(std::string(spec.message));

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

HttpServerResponse BuildJsonErrorResponse(int status_code, std::string_view message, std::string_view reason) {
  HttpServerResponse response;
  response.status_code = status_code;
  response.headers[std::string(core::http::kContentTypeHeaderName)] =
      std::string(core::http::kContentTypeApplicationJson);
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  response.body = ErrorBody({.status_code = status_code, .message = message, .reason = reason});
  return response;
}

void AddActivatedExtensionsHeader(const std::vector<std::string>& activated_extensions, HttpServerResponse* response) {
  if (activated_extensions.empty() || response == nullptr) {
    return;
  }
  response->headers[std::string(core::Extensions::kHeaderName)] = core::Extensions::Format(activated_extensions);
}

HttpServerResponse BuildValidatedErrorResponse(int status_code, std::string_view message, std::string_view reason,
                                               const std::vector<std::string>& activated_extensions) {
  auto response = BuildJsonErrorResponse(status_code, message, reason);
  AddActivatedExtensionsHeader(activated_extensions, &response);
  return response;
}

std::string_view ProtocolCodeToRestReason(std::string_view protocol_code) {
  if (protocol_code == core::protocol_codes::kExtendedAgentCardNotConfigured) {
    return "EXTENDED_AGENT_CARD_NOT_CONFIGURED";
  }
  if (protocol_code == core::protocol_codes::kUnsupportedOperation) {
    return "UNSUPPORTED_OPERATION";
  }
  if (protocol_code == core::protocol_codes::kContentTypeNotSupported) {
    return "CONTENT_TYPE_NOT_SUPPORTED";
  }
  if (protocol_code == core::protocol_codes::kInvalidAgentResponse) {
    return "INVALID_AGENT_RESPONSE";
  }
  if (protocol_code == core::protocol_codes::kExtensionSupportRequired) {
    return "EXTENSION_SUPPORT_REQUIRED";
  }
  if (protocol_code == core::protocol_codes::kVersionNotSupported) {
    return "VERSION_NOT_SUPPORTED";
  }
  return "REMOTE_PROTOCOL_ERROR";
}

std::string_view RestReasonFromError(const core::Error& error) {
  const auto& protocol_code = error.protocol_code();
  if (protocol_code.has_value()) {
    return ProtocolCodeToRestReason(*protocol_code);
  }
  if (error.code() == core::ErrorCode::kUnsupportedVersion) {
    return "VERSION_NOT_SUPPORTED";
  }
  if (error.code() == core::ErrorCode::kInternal || error.code() == core::ErrorCode::kSerialization) {
    return "INTERNAL";
  }
  return "INVALID_ARGUMENT";
}

int HttpStatusFromError(const core::Error& error) {
  const auto& http_status = error.http_status();
  if (http_status.has_value()) {
    return *http_status;
  }
  if (error.code() == core::ErrorCode::kInternal || error.code() == core::ErrorCode::kSerialization) {
    return core::http::kStatusInternalServerError;
  }
  if (error.code() == core::ErrorCode::kNetwork) {
    return core::http::kStatusBadGateway;
  }
  return core::http::kStatusBadRequest;
}

std::uint64_t ComputeEtagHash(std::string_view data) {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const char ch : data) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= kFnvPrime;
  }
  return hash;
}

std::string FormatHttpDate(std::chrono::system_clock::time_point time_point) {
  const std::time_t timestamp = std::chrono::system_clock::to_time_t(time_point);
  const std::tm* utc_tm = std::gmtime(&timestamp);
  if (utc_tm == nullptr) {
    return {};
  }
  std::ostringstream stream;
  stream << std::put_time(utc_tm, "%a, %d %b %Y %H:%M:%S GMT");
  return stream.str();
}

std::string BuildQuotedEtag(std::uint64_t hash_value) {
  const std::string hash = std::to_string(hash_value);
  std::string etag;
  etag.reserve(hash.size() + 2);
  etag.push_back('"');
  etag.append(hash);
  etag.push_back('"');
  return etag;
}

void ApplyAgentCardCacheHeaders(const std::optional<RestServerTransportOptions::AgentCardCacheSettings>& settings,
                                HttpServerResponse* response) {
  if (settings.has_value() && settings->cache_control.has_value()) {
    response->headers["Cache-Control"] = *settings->cache_control;
  }
  if (!settings.has_value() || !settings->last_modified.has_value()) {
    return;
  }

  const std::string formatted = FormatHttpDate(*settings->last_modified);
  if (!formatted.empty()) {
    response->headers["Last-Modified"] = formatted;
  }
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

core::Result<void> ParseQueryString(std::string_view raw, std::unordered_map<std::string, std::string>* out) {
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
    const auto part = raw.substr(start, end == std::string_view::npos ? raw.size() - start : end - start);
    if (!part.empty()) {
      const std::size_t split = part.find('=');
      const auto key_raw = part.substr(0, split);
      const auto value_raw = split == std::string_view::npos ? std::string_view{} : part.substr(split + 1);

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

core::Result<bool> HasExtendedAgentCardView(std::string_view query) {
  std::unordered_map<std::string, std::string> query_params;
  const auto parsed = ParseQueryString(query, &query_params);
  if (!parsed.ok()) {
    return parsed.error();
  }

  const auto view = query_params.find(std::string(kAgentCardViewQueryKey));
  return view != query_params.end() && view->second == kExtendedAgentCardViewQueryValue;
}

bool PathStartsWithBasePath(std::string_view path, std::string_view base_path) {
  return !base_path.empty() && base_path != "/" && path.starts_with(base_path) &&
         (path.size() == base_path.size() || path[base_path.size()] == '/');
}

std::optional<std::string> ExtractTenantFromRelativeExtendedAgentCardPath(std::string_view path) {
  if (!path.starts_with('/') || !path.ends_with(RestServerTransport::kExtendedAgentCardPath)) {
    return std::nullopt;
  }

  const std::size_t tenant_end = path.size() - RestServerTransport::kExtendedAgentCardPath.size();
  if (tenant_end <= 1 || path[tenant_end] != '/') {
    return std::nullopt;
  }

  const std::string_view tenant = path.substr(1, tenant_end - 1);
  if (tenant.empty() || tenant.find('/') != std::string_view::npos) {
    return std::nullopt;
  }
  return std::string(tenant);
}

std::optional<std::string> ExtractTenantFromExtendedAgentCardPath(std::string_view path, std::string_view base_path) {
  if (auto tenant = ExtractTenantFromRelativeExtendedAgentCardPath(path); tenant.has_value()) {
    return tenant;
  }
  if (!PathStartsWithBasePath(path, base_path)) {
    return std::nullopt;
  }

  const std::string_view relative_path = path.substr(base_path.size());
  return ExtractTenantFromRelativeExtendedAgentCardPath(relative_path);
}

}  // namespace

RestServerTransport::RestServerTransport(Dispatcher* dispatcher, lf::a2a::v1::AgentCard agent_card,
                                         RestServerTransportOptions options)
    : dispatcher_(dispatcher),
      transport_(dispatcher),
      agent_card_(std::move(agent_card)),
      options_(std::move(options)),
      required_extensions_validator_(options_.required_extensions) {
  options_.rest_api_base_path = NormalizeBasePath(options_.rest_api_base_path);
  for (auto& iface : *agent_card_.mutable_supported_interfaces()) {
    if (iface.protocol_version().empty()) {
      iface.set_protocol_version(core::Version::HeaderValue());
    }
  }
}

core::Result<HttpServerResponse> RestServerTransport::Handle(const HttpServerRequest& request) const {
  if (request.target.empty() || request.target.front() != '/') {
    return core::Error::Validation("HTTP request target must start with '/'");
  }

  const auto query_start = request.target.find('?');
  const std::string_view path = query_start == std::string::npos
                                    ? std::string_view(request.target)
                                    : std::string_view(request.target).substr(0, query_start);
  const std::string_view query =
      query_start == std::string::npos ? std::string_view{} : std::string_view(request.target).substr(query_start + 1);
  const bool is_base_path_extended_agent_card =
      options_.rest_api_base_path != "/" && path == options_.rest_api_base_path + std::string(kExtendedAgentCardPath);

  if (path == kAgentCardPath) {
    const auto extended_view = HasExtendedAgentCardView(query);
    if (!extended_view.ok()) {
      return extended_view.error();
    }
    if (extended_view.value()) {
      return HandleExtendedAgentCard(request, {});
    }
    return HandleAgentCard(request);
  }
  if (path == kLegacyAgentCardPath) {
    return HandleAgentCard(request);
  }
  if (path == kExtendedAgentCardPath || is_base_path_extended_agent_card) {
    return HandleExtendedAgentCard(request, {});
  }
  if (const auto tenant = ExtractTenantFromExtendedAgentCardPath(path, options_.rest_api_base_path);
      tenant.has_value()) {
    return HandleExtendedAgentCard(request, *tenant);
  }

  const auto version = ValidateVersionHeader(request);
  if (!version.ok()) {
    return BuildJsonErrorResponse(core::http::kStatusBadRequest, version.error().message(), "VERSION_NOT_SUPPORTED");
  }

  const auto extensions = required_extensions_validator_.Validate(request.headers);
  if (!extensions.ok()) {
    return BuildJsonErrorResponse(core::http::kStatusBadRequest, extensions.error().message(),
                                  "EXTENSION_SUPPORT_REQUIRED");
  }

  const auto rest_request = BuildRestRequest(request);
  if (!rest_request.ok()) {
    return BuildValidatedErrorResponse(core::http::kStatusNotFound, "No matching route or request was malformed",
                                       "UNSUPPORTED_OPERATION", extensions.value());
  }

  const auto rest_response = transport_.Handle(rest_request.value());
  if (!rest_response.ok()) {
    return rest_response.error();
  }
  return ToHttpResponse(rest_response.value(), extensions.value());
}

core::Result<RestRequest> RestServerTransport::BuildRestRequest(const HttpServerRequest& request) const {
  const auto query_start = request.target.find('?');
  std::string path = query_start == std::string::npos ? request.target : request.target.substr(0, query_start);

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
  rest_request.headers = request.headers;
  rest_request.context.remote_address = request.remote_address.empty()
                                            ? std::optional<std::string>{}
                                            : std::optional<std::string>(request.remote_address);
  rest_request.context.client_headers = request.headers;
  rest_request.context.auth_metadata = ExtractAuthMetadata(request.headers);

  if (query_start != std::string::npos) {
    const auto parsed =
        ParseQueryString(std::string_view(request.target).substr(query_start + 1), &rest_request.query_params);
    if (!parsed.ok()) {
      return parsed.error();
    }
  }

  return rest_request;
}

core::Result<void> RestServerTransport::ValidateVersionHeader(const HttpServerRequest& request) const {
  const auto version_header = core::http::FindHeaderValue(request.headers, core::Version::kHeaderName);
  const std::string version = version_header.has_value() ? std::string(*version_header) : std::string();
  if (version.empty()) {
    if (options_.require_version_header) {
      return core::Error::UnsupportedVersion("Missing required A2A-Version header");
    }
    return {};
  }
  if (!core::Version::IsSupported(version)) {
    return core::Error::UnsupportedVersion("Unsupported A2A-Version header value").WithProtocolCode(version);
  }
  return {};
}

core::Result<HttpServerResponse> RestServerTransport::HandleAgentCard(const HttpServerRequest& request) const {
  if (request.method != "GET") {
    return BuildJsonErrorResponse(core::http::kStatusNotFound, "No matching route or request was malformed",
                                  "UNSUPPORTED_OPERATION");
  }

  const auto card = BuildNormalizedAgentCard(agent_card_, options_.include_legacy_transport_fields);
  if (!card.ok()) {
    return card.error();
  }

  const auto normalized = core::MessageToJson(card.value());
  if (!normalized.ok()) {
    return normalized.error();
  }

  HttpServerResponse response;
  response.status_code = core::http::kStatusOk;
  response.headers[std::string(core::http::kContentTypeHeaderName)] =
      std::string(core::http::kContentTypeApplicationJson);
  ApplyAgentCardCacheHeaders(options_.agent_card_cache_settings, &response);
  response.headers["ETag"] = BuildQuotedEtag(ComputeEtagHash(normalized.value()));
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  response.body = normalized.value();
  return response;
}

core::Result<HttpServerResponse> RestServerTransport::HandleExtendedAgentCard(const HttpServerRequest& request,
                                                                              std::string_view tenant) const {
  if (request.method != "GET") {
    return BuildJsonErrorResponse(core::http::kStatusNotFound, "No matching route or request was malformed",
                                  "UNSUPPORTED_OPERATION");
  }
  const auto version = ValidateVersionHeader(request);
  if (!version.ok()) {
    return BuildJsonErrorResponse(core::http::kStatusBadRequest, version.error().message(), "VERSION_NOT_SUPPORTED");
  }
  const auto extensions = required_extensions_validator_.Validate(request.headers);
  if (!extensions.ok()) {
    return BuildJsonErrorResponse(core::http::kStatusBadRequest, extensions.error().message(),
                                  "EXTENSION_SUPPORT_REQUIRED");
  }
  RequestContext context;
  context.remote_address = request.remote_address.empty() ? std::optional<std::string>{}
                                                          : std::optional<std::string>(request.remote_address);
  context.client_headers = request.headers;
  context.auth_metadata = ExtractAuthMetadata(request.headers);
  lf::a2a::v1::GetExtendedAgentCardRequest card_request;
  if (!tenant.empty()) {
    card_request.set_tenant(std::string(tenant));
  }
  const auto dispatch = dispatcher_->Dispatch(
      {.operation = DispatcherOperation::kGetExtendedAgentCard, .payload = card_request}, context);
  if (!dispatch.ok()) {
    return BuildValidatedErrorResponse(HttpStatusFromError(dispatch.error()), dispatch.error().message(),
                                       RestReasonFromError(dispatch.error()), extensions.value());
  }
  const auto* payload = std::get_if<lf::a2a::v1::AgentCard>(&dispatch.value().payload());
  if (payload == nullptr) {
    return core::Error::Internal("GetExtendedAgentCard dispatch returned an unexpected payload");
  }

  const auto card = BuildNormalizedAgentCard(*payload, options_.include_legacy_transport_fields);
  if (!card.ok()) {
    return card.error();
  }

  const auto normalized = core::MessageToJson(card.value());
  if (!normalized.ok()) {
    return normalized.error();
  }

  HttpServerResponse response;
  response.status_code = core::http::kStatusOk;
  response.headers[std::string(core::http::kContentTypeHeaderName)] =
      std::string(core::http::kContentTypeApplicationJson);
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  AddActivatedExtensionsHeader(extensions.value(), &response);
  response.body = normalized.value();
  return response;
}

HttpServerResponse RestServerTransport::ToHttpResponse(const RestResponse& response,
                                                       const std::vector<std::string>& activated_extensions) {
  HttpServerResponse http_response;
  http_response.status_code = response.http_status;
  http_response.headers = response.headers;
  http_response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  http_response.body = response.body;
  http_response.stream_writer = response.stream_writer;
  AddActivatedExtensionsHeader(activated_extensions, &http_response);
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
