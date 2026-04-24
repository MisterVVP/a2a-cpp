#include "a2a/server/rest_transport.h"

#include <algorithm>
#include <cctype>
#include <string>

#include "a2a/core/version.h"

namespace a2a::server {
namespace {

constexpr int kHttpStatusOk = 200;
constexpr int kHttpStatusBadRequest = 400;
constexpr int kHttpStatusInternalServerError = 500;

std::string ToLower(std::string_view value) {
  std::string lowered(value);
  std::ranges::transform(lowered, lowered.begin(),
                         [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return lowered;
}

std::optional<std::string> FindHeaderValue(
    const std::unordered_map<std::string, std::string>& headers, std::string_view header_name) {
  const std::string lowered_name = ToLower(header_name);
  for (const auto& [name, value] : headers) {
    if (ToLower(name) == lowered_name) {
      return value;
    }
  }
  return std::nullopt;
}

}  // namespace

RestAdapter::RestAdapter(const Dispatcher* dispatcher) : dispatcher_(dispatcher) {}

RestPipelineResponse RestAdapter::Handle(
    const DispatchRequest& request,
    const std::unordered_map<std::string, std::string>& request_headers,
    RequestContext& context) const {
  const auto version = FindHeaderValue(request_headers, core::Version::kHeaderName);
  if (!version.has_value()) {
    RestPipelineResponse response;
    response.status_code = kHttpStatusBadRequest;
    response.error = core::Error::Validation("Missing required A2A-Version request header")
                         .WithTransport("http")
                         .WithHttpStatus(kHttpStatusBadRequest);
    return response;
  }

  if (!core::Version::IsSupported(*version)) {
    RestPipelineResponse response;
    response.status_code = kHttpStatusBadRequest;
    response.error = core::Error::UnsupportedVersion("Unsupported A2A-Version request header")
                         .WithTransport("http")
                         .WithProtocolCode(*version)
                         .WithHttpStatus(kHttpStatusBadRequest);
    return response;
  }

  if (dispatcher_ == nullptr) {
    RestPipelineResponse response;
    response.status_code = kHttpStatusInternalServerError;
    response.error = core::Error::Internal("REST adapter dispatcher is not configured")
                         .WithTransport("http")
                         .WithHttpStatus(kHttpStatusInternalServerError);
    return response;
  }

  auto dispatch_result = dispatcher_->Dispatch(request, context);
  if (!dispatch_result.ok()) {
    RestPipelineResponse response;
    response.status_code = dispatch_result.error().http_status().value_or(kHttpStatusBadRequest);
    response.error = dispatch_result.error();
    return response;
  }

  RestPipelineResponse response;
  response.status_code = kHttpStatusOk;
  response.headers[std::string(core::Version::kHeaderName)] = core::Version::HeaderValue();
  response.payload = std::move(dispatch_result.value());
  return response;
}

}  // namespace a2a::server
