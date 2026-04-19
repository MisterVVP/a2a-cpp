#include "a2a/client/discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"

namespace a2a::client {

namespace {

constexpr int kHttpStatusOkMin = 200;
constexpr int kHttpStatusOkMax = 299;
constexpr int kHttpStatusNotFound = 404;

std::string Trim(std::string_view input) {
  std::string value(input);
  const auto begin =
      std::ranges::find_if_not(value, [](unsigned char ch) { return std::isspace(ch) != 0; });
  const auto end = std::ranges::find_if_not(std::ranges::reverse_view(value), [](unsigned char ch) {
                     return std::isspace(ch) != 0;
                   }).base();
  if (begin >= end) {
    return {};
  }
  return {begin, end};
}

bool HasHttpScheme(std::string_view url) {
  return url.starts_with("http://") || url.starts_with("https://");
}

bool HasGrpcScheme(std::string_view url) {
  return url.starts_with("grpc://") || url.starts_with("grpcs://") || url.starts_with("dns:///");
}

bool HasHostPortShape(std::string_view endpoint) {
  return endpoint.find(':') != std::string_view::npos;
}

bool IsValidInterfaceEndpoint(lf::a2a::v1::TransportProtocol transport, std::string_view endpoint) {
  switch (transport) {
    case lf::a2a::v1::TRANSPORT_PROTOCOL_REST:
    case lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC:
      return HasHttpScheme(endpoint);
    case lf::a2a::v1::TRANSPORT_PROTOCOL_GRPC:
      return HasGrpcScheme(endpoint) || HasHttpScheme(endpoint) || HasHostPortShape(endpoint);
    case lf::a2a::v1::TRANSPORT_PROTOCOL_UNSPECIFIED:
    case lf::a2a::v1::TransportProtocol_INT_MIN_SENTINEL_DO_NOT_USE_:
    case lf::a2a::v1::TransportProtocol_INT_MAX_SENTINEL_DO_NOT_USE_:
      return false;
  }
  return false;
}

PreferredTransport ToPreferredTransport(lf::a2a::v1::TransportProtocol transport) {
  switch (transport) {
    case lf::a2a::v1::TRANSPORT_PROTOCOL_REST:
      return PreferredTransport::kRest;
    case lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC:
      return PreferredTransport::kJsonRpc;
    case lf::a2a::v1::TRANSPORT_PROTOCOL_GRPC:
      return PreferredTransport::kGrpc;
    case lf::a2a::v1::TRANSPORT_PROTOCOL_UNSPECIFIED:
    case lf::a2a::v1::TransportProtocol_INT_MIN_SENTINEL_DO_NOT_USE_:
    case lf::a2a::v1::TransportProtocol_INT_MAX_SENTINEL_DO_NOT_USE_:
      break;
  }
  return PreferredTransport::kRest;
}

std::optional<lf::a2a::v1::TransportProtocol> ToWireTransport(PreferredTransport transport) {
  switch (transport) {
    case PreferredTransport::kRest:
      return lf::a2a::v1::TRANSPORT_PROTOCOL_REST;
    case PreferredTransport::kJsonRpc:
      return lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC;
    case PreferredTransport::kGrpc:
      return lf::a2a::v1::TRANSPORT_PROTOCOL_GRPC;
  }
  return std::nullopt;
}

}  // namespace

DiscoveryClient::DiscoveryClient(HttpFetcher fetcher, std::chrono::seconds cache_ttl)
    : fetcher_(std::move(fetcher)), cache_ttl_(cache_ttl) {}

core::Result<lf::a2a::v1::AgentCard> DiscoveryClient::Fetch(std::string_view base_url) {
  const auto discovery_url = BuildDiscoveryUrl(base_url);
  if (!discovery_url.ok()) {
    return discovery_url.error();
  }

  const auto now = std::chrono::steady_clock::now();
  const auto cached = cache_.find(discovery_url.value());
  if (cached != cache_.end() && cached->second.expires_at >= now) {
    return cached->second.card;
  }

  const auto response = fetcher_(discovery_url.value());
  if (!response.ok()) {
    return response.error();
  }
  if (response.value().status_code == kHttpStatusNotFound) {
    return core::Error::RemoteProtocol("Agent Card not found at well-known discovery endpoint")
        .WithTransport("http")
        .WithHttpStatus(kHttpStatusNotFound);
  }
  if (response.value().status_code < kHttpStatusOkMin ||
      response.value().status_code > kHttpStatusOkMax) {
    return core::Error::RemoteProtocol("Agent Card discovery failed")
        .WithTransport("http")
        .WithHttpStatus(response.value().status_code);
  }

  lf::a2a::v1::AgentCard card;
  const auto parse = core::JsonToMessage(response.value().body, &card);
  if (!parse.ok()) {
    return parse.error();
  }

  const auto validation = ValidateAgentCard(card);
  if (!validation.ok()) {
    return validation.error();
  }

  cache_[discovery_url.value()] = CacheEntry{.card = card, .expires_at = now + cache_ttl_};
  return card;
}

core::Result<std::string> DiscoveryClient::BuildDiscoveryUrl(std::string_view base_url) {
  std::string normalized = Trim(base_url);
  if (normalized.empty()) {
    return core::Error::Validation("Base URL is required for Agent Card discovery");
  }
  if (!HasHttpScheme(normalized)) {
    return core::Error::Validation("Base URL must start with http:// or https://");
  }

  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }
  return normalized + "/.well-known/agent-card.json";
}

core::Result<void> DiscoveryClient::ValidateAgentCard(const lf::a2a::v1::AgentCard& card) {
  if (card.protocol_version().empty()) {
    return core::Error::Validation("Agent Card protocol_version is required");
  }
  if (!core::Version::IsSupported(card.protocol_version())) {
    return core::Error::UnsupportedVersion("Only A2A protocol version 1.0 is supported");
  }
  if (card.supported_interfaces().empty()) {
    return core::Error::Validation("Agent Card must include at least one supported interface");
  }

  for (const auto& iface : card.supported_interfaces()) {
    if (iface.transport() == lf::a2a::v1::TRANSPORT_PROTOCOL_UNSPECIFIED) {
      return core::Error::Validation("Agent Card contains an interface with unspecified transport");
    }
    if (iface.url().empty()) {
      return core::Error::Validation("Agent Card contains an interface without a URL");
    }
    if (!IsValidInterfaceEndpoint(iface.transport(), iface.url())) {
      return core::Error::Validation("Agent Card interface endpoint is invalid for its transport");
    }
    for (const auto& requirement : iface.security_requirements()) {
      if (!card.security_schemes().contains(requirement)) {
        return core::Error::Validation(
            "Agent Card interface references an unknown security scheme: " + requirement);
      }
    }
  }

  for (const auto& requirement : card.default_security_requirements()) {
    if (!card.security_schemes().contains(requirement)) {
      return core::Error::Validation("Agent Card default security requirement is not defined: " +
                                     requirement);
    }
  }
  return {};
}

core::Result<ResolvedInterface> AgentCardResolver::SelectPreferredInterface(
    const lf::a2a::v1::AgentCard& card, PreferredTransport preferred) {
  const auto preferred_wire = ToWireTransport(preferred);
  if (!preferred_wire.has_value()) {
    return core::Error::Validation("Invalid preferred transport requested");
  }

  std::array<lf::a2a::v1::TransportProtocol, 3> order = {preferred_wire.value(),
                                                         lf::a2a::v1::TRANSPORT_PROTOCOL_REST,
                                                         lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC};
  if (preferred_wire.value() == lf::a2a::v1::TRANSPORT_PROTOCOL_REST) {
    order[1] = lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC;
    order[2] = lf::a2a::v1::TRANSPORT_PROTOCOL_GRPC;
  } else if (preferred_wire.value() == lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC) {
    order[1] = lf::a2a::v1::TRANSPORT_PROTOCOL_REST;
    order[2] = lf::a2a::v1::TRANSPORT_PROTOCOL_GRPC;
  } else {
    order[1] = lf::a2a::v1::TRANSPORT_PROTOCOL_REST;
    order[2] = lf::a2a::v1::TRANSPORT_PROTOCOL_JSON_RPC;
  }

  for (const auto transport : order) {
    for (const auto& iface : card.supported_interfaces()) {
      if (iface.transport() != transport) {
        continue;
      }
      const auto valid = ValidateInterface(iface);
      if (!valid.ok()) {
        continue;
      }

      ResolvedInterface resolved;
      resolved.transport = ToPreferredTransport(transport);
      resolved.url = iface.url();
      if (iface.security_requirements().empty()) {
        resolved.security_requirements.insert(resolved.security_requirements.end(),
                                              card.default_security_requirements().begin(),
                                              card.default_security_requirements().end());
      } else {
        resolved.security_requirements.insert(resolved.security_requirements.end(),
                                              iface.security_requirements().begin(),
                                              iface.security_requirements().end());
      }
      for (const auto& name : resolved.security_requirements) {
        const auto scheme = card.security_schemes().find(name);
        if (scheme != card.security_schemes().end()) {
          resolved.security_schemes.emplace(name, scheme->second);
        }
      }
      return resolved;
    }
  }

  return core::Error::Validation("No usable interface found for this Agent Card");
}

core::Result<void> AgentCardResolver::ValidateInterface(const lf::a2a::v1::AgentInterface& iface) {
  if (iface.transport() == lf::a2a::v1::TRANSPORT_PROTOCOL_UNSPECIFIED) {
    return core::Error::Validation("Unsupported transport");
  }
  if (iface.url().empty()) {
    return core::Error::Validation("Missing interface URL");
  }
  if (!IsValidInterfaceEndpoint(iface.transport(), iface.url())) {
    return core::Error::Validation("Interface endpoint is invalid for its transport");
  }
  return {};
}

}  // namespace a2a::client
