// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "http_json_transport_internal.h"

#include <google/protobuf/struct.pb.h>

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/http_utils.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::client::http_json_internal {
namespace {

constexpr char kHttpTransportName[] = "http";
constexpr std::string_view kUnsupportedVersionMessage = "Server returned unsupported A2A-Version header";
constexpr std::string_view kHttpRequestFailurePrefix = "HTTP request failed for ";
constexpr std::string_view kProtocolCodeMemberName = "code";

}  // namespace

a2a::http::Request ToSharedHttpRequest(const HttpRequest& request) {
  a2a::http::Request shared_request;
  shared_request.method = request.method;
  shared_request.url = request.url;
  shared_request.body = request.body;
  shared_request.timeout = request.timeout;
  shared_request.headers.reserve(request.headers.size());
  for (const auto& [name, value] : request.headers) {
    shared_request.headers.push_back(a2a::http::Header{.name = name, .value = value});
  }
  return shared_request;
}

HttpClientResponse ToClientHttpResponse(a2a::http::Response response) {
  HttpClientResponse client_response;
  client_response.status_code = response.status_code;
  client_response.body = std::move(response.body);
  client_response.headers.reserve(response.headers.size());
  for (auto& header : response.headers) {
    client_response.headers.insert_or_assign(std::move(header.name), std::move(header.value));
  }
  return client_response;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::string JoinUrl(std::string_view interface_base_url, std::string_view rpc_endpoint) {
  std::string base(interface_base_url);
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  if (rpc_endpoint.empty()) {
    return base;
  }
  if (!rpc_endpoint.starts_with('/')) {
    base.push_back('/');
  }
  base.append(rpc_endpoint);
  return base;
}

core::Result<void> ValidateResponseVersion(const HttpClientResponse& response) {
  const auto version = core::http::FindHeaderValue(response.headers, core::Version::kHeaderName);
  if (!version.has_value()) {
    return {};
  }
  if (!core::Version::IsSupported(version.value())) {
    return core::Error::UnsupportedVersion(std::string(kUnsupportedVersionMessage))
        .WithTransport(kHttpTransportName)
        .WithProtocolCode(std::string(version.value()));
  }
  return {};
}

core::Error BuildHttpError(std::string_view method, std::string_view endpoint, const HttpClientResponse& response) {
  std::ostringstream stream;
  stream << kHttpRequestFailurePrefix << method << " " << endpoint;
  if (!response.body.empty()) {
    stream << ": " << response.body;
  }

  core::Error error = core::Error::RemoteProtocol(stream.str()).WithTransport(kHttpTransportName);
  error = error.WithHttpStatus(response.status_code);

  if (!response.body.empty() && response.body.front() == '{') {
    google::protobuf::Struct status_payload;
    if (core::JsonToMessage(response.body, &status_payload, {.ignore_unknown_fields = true}).ok()) {
      const auto code = status_payload.fields().find(kProtocolCodeMemberName);
      if (code != status_payload.fields().end() &&
          code->second.kind_case() == ::google::protobuf::Value::kStringValue) {
        error = error.WithProtocolCode(code->second.string_value());
      }
    }
  }
  return error;
}

std::string BuildTaskPath(std::string_view task_id) {
  std::string path;
  path.reserve(EndpointMap::kTaskCollection.size() + 1 + task_id.size());
  path += EndpointMap::kTaskCollection;
  path += '/';
  path += task_id;
  return path;
}

}  // namespace a2a::client::http_json_internal
