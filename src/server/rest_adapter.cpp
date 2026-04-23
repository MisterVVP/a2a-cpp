#include "a2a/server/rest_adapter.h"

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::server {
namespace {

constexpr std::string_view kWellKnownPath = "/.well-known/agent-card.json";
constexpr std::string_view kHttpMethodGet = "GET";
constexpr std::string_view kContentTypeHeaderName = "Content-Type";
constexpr std::string_view kApplicationJson = "application/json";
constexpr int kStatusOk = 200;
constexpr int kStatusNotFound = 404;
constexpr int kStatusInternalServerError = 500;

std::string NormalizeBaseUrl(std::string_view url) {
  std::string normalized(url);
  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized;
}

void KeepDefinedSecurityRequirements(const std::unordered_set<std::string>& scheme_names,
                                     google::protobuf::RepeatedPtrField<std::string>* requirements) {
  if (requirements == nullptr) {
    return;
  }

  std::vector<std::string> filtered;
  filtered.reserve(static_cast<std::size_t>(requirements->size()));
  for (const std::string& requirement : *requirements) {
    if (scheme_names.contains(requirement)) {
      filtered.push_back(requirement);
    }
  }

  requirements->Clear();
  for (const std::string& requirement : filtered) {
    requirements->Add(std::string(requirement));
  }
}

RestResponse MakeJsonErrorResponse(std::string body) {
  RestResponse response;
  response.status_code = kStatusInternalServerError;
  response.body = std::move(body);
  response.headers.emplace(std::string(kContentTypeHeaderName), std::string(kApplicationJson));
  return response;
}

}  // namespace

RestAdapter::RestAdapter(RestAdapterConfig config) : config_(std::move(config)) {}

RestResponse RestAdapter::Handle(const RestRequest& request, const RequestContext& context) const {
  if (request.method == kHttpMethodGet && request.path == kWellKnownPath) {
    return HandleAgentCard(context);
  }

  RestResponse not_found;
  not_found.status_code = kStatusNotFound;
  return not_found;
}

RestResponse RestAdapter::HandleAgentCard(const RequestContext& context) const {
  if (!config_.agent_card_provider) {
    return MakeJsonErrorResponse(R"({"error":"AgentCardProvider is not configured"})");
  }

  const auto provided = config_.agent_card_provider(context);
  if (!provided.ok()) {
    return MakeJsonErrorResponse(R"({"error":"AgentCardProvider failed"})");
  }

  lf::a2a::v1::AgentCard card = provided.value();
  card.set_protocol_version(std::string(core::Version::kProtocolVersion));

  const std::string normalized_base_url = NormalizeBaseUrl(config_.rest_base_url);
  if (!normalized_base_url.empty()) {
    bool has_matching_rest = false;
    for (auto& iface : *card.mutable_supported_interfaces()) {
      if (iface.transport() == lf::a2a::v1::TRANSPORT_PROTOCOL_REST &&
          iface.url() == normalized_base_url) {
        has_matching_rest = true;
        break;
      }
    }

    if (!has_matching_rest) {
      auto* rest = card.add_supported_interfaces();
      rest->set_transport(lf::a2a::v1::TRANSPORT_PROTOCOL_REST);
      rest->set_url(normalized_base_url);
      for (const std::string& requirement : card.default_security_requirements()) {
        rest->add_security_requirements(requirement);
      }
    }
  }

  std::unordered_set<std::string> scheme_names;
  scheme_names.reserve(static_cast<std::size_t>(card.security_schemes_size()));
  for (const auto& [name, _] : card.security_schemes()) {
    scheme_names.insert(name);
  }

  KeepDefinedSecurityRequirements(scheme_names, card.mutable_default_security_requirements());
  for (auto& iface : *card.mutable_supported_interfaces()) {
    KeepDefinedSecurityRequirements(scheme_names, iface.mutable_security_requirements());
  }

  const auto json = core::MessageToJson(card);
  if (!json.ok()) {
    return MakeJsonErrorResponse(R"({"error":"Unable to serialize Agent Card"})");
  }

  RestResponse response;
  response.status_code = kStatusOk;
  response.body = json.value();
  response.headers.emplace(std::string(kContentTypeHeaderName), std::string(kApplicationJson));
  response.headers.emplace(std::string(core::Version::kHeaderName), core::Version::HeaderValue());
  return response;
}

}  // namespace a2a::server
