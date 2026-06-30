// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/client/discovery.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <ranges>
#include <string>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/protojson.h"
#include "a2a/core/version.h"
#include "a2a/http/http_client.h"

namespace a2a::client {

namespace {

constexpr int kHttpStatusOkMin = 200;
constexpr int kHttpStatusOkMax = 299;
constexpr int kHttpStatusNotFound = 404;
constexpr std::string_view kDiscoveryGetMethod = "GET";
constexpr std::string_view kExtendedAgentCardPath = "/extendedAgentCard";

std::string Trim(std::string_view input) {
  std::string value(input);
  const auto begin = std::ranges::find_if_not(value, [](unsigned char ch) { return std::isspace(ch) != 0; });
  const auto end = std::ranges::find_if_not(std::ranges::reverse_view(value), [](unsigned char ch) {
                     return std::isspace(ch) != 0;
                   }).base();
  if (begin >= end) {
    return {};
  }
  return {begin, end};
}

HttpResponse ToDiscoveryHttpResponse(a2a::http::Response response) {
  return HttpResponse{.status_code = response.status_code, .body = std::move(response.body)};
}

bool HasHttpScheme(std::string_view url) { return url.starts_with("http://") || url.starts_with("https://"); }

bool HasGrpcScheme(std::string_view url) {
  return url.starts_with("grpc://") || url.starts_with("grpcs://") || url.starts_with("dns:///");
}

bool HasHostPortShape(std::string_view endpoint) { return endpoint.find(':') != std::string_view::npos; }

using a2a::core::protocol_bindings::kGrpc;
using a2a::core::protocol_bindings::kHttpJson;
using a2a::core::protocol_bindings::kJsonRpc;

struct InterfaceEndpoint final {
  std::string_view protocol_binding;
  std::string_view endpoint;
};

bool IsValidInterfaceEndpoint(const InterfaceEndpoint& candidate) {
  if (candidate.protocol_binding == kHttpJson || candidate.protocol_binding == kJsonRpc) {
    return HasHttpScheme(candidate.endpoint);
  }
  if (candidate.protocol_binding == kGrpc) {
    return HasGrpcScheme(candidate.endpoint) || HasHttpScheme(candidate.endpoint) ||
           HasHostPortShape(candidate.endpoint);
  }
  return false;
}

PreferredTransport ToPreferredTransport(std::string_view protocol_binding) {
  if (protocol_binding == kHttpJson) {
    return PreferredTransport::kRest;
  }
  if (protocol_binding == kJsonRpc) {
    return PreferredTransport::kJsonRpc;
  }
  return PreferredTransport::kGrpc;
}

std::optional<std::string_view> ToWireTransport(PreferredTransport transport) {
  switch (transport) {
    case PreferredTransport::kRest:
      return kHttpJson;
    case PreferredTransport::kJsonRpc:
      return kJsonRpc;
    case PreferredTransport::kGrpc:
      return kGrpc;
  }
  return std::nullopt;
}

}  // namespace

HttpFetcher MakeDefaultHttpFetcher() {
  return [client = a2a::http::Client{}](std::string_view url) -> core::Result<HttpResponse> {
    a2a::http::Request request;
    request.method = std::string(kDiscoveryGetMethod);
    request.url = std::string(url);
    request.headers.push_back({std::string(core::Version::kHeaderName), core::Version::HeaderValue()});
    auto response = client.SendRequest(request);
    if (!response.ok()) {
      return response.error();
    }
    return ToDiscoveryHttpResponse(std::move(response.value()));
  };
}

DiscoveryClient::DiscoveryClient(HttpFetcher fetcher, std::chrono::seconds cache_ttl)
    : fetcher_(std::move(fetcher)), cache_ttl_(cache_ttl) {}

DiscoveryClient DiscoveryClient::CreateDefault(std::chrono::seconds cache_ttl) {
  return DiscoveryClient(MakeDefaultHttpFetcher(), cache_ttl);
}

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
  if (response.value().status_code < kHttpStatusOkMin || response.value().status_code > kHttpStatusOkMax) {
    return core::Error::RemoteProtocol("Agent Card discovery failed")
        .WithTransport("http")
        .WithHttpStatus(response.value().status_code);
  }

  lf::a2a::v1::AgentCard card;
  const auto parse = core::JsonToMessage(response.value().body, &card, {.ignore_unknown_fields = true});
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

core::Result<lf::a2a::v1::AgentCard> DiscoveryClient::FetchExtendedAgentCard(std::string_view base_url) {
  const auto discovery_url = BuildExtendedDiscoveryUrl(base_url);
  if (!discovery_url.ok()) {
    return discovery_url.error();
  }

  const auto response = fetcher_(discovery_url.value());
  if (!response.ok()) {
    return response.error();
  }
  if (response.value().status_code == kHttpStatusNotFound) {
    return core::Error::RemoteProtocol("Extended Agent Card not found at discovery endpoint")
        .WithTransport("http")
        .WithHttpStatus(kHttpStatusNotFound);
  }
  if (response.value().status_code < kHttpStatusOkMin || response.value().status_code > kHttpStatusOkMax) {
    return core::Error::RemoteProtocol("Extended Agent Card discovery failed")
        .WithTransport("http")
        .WithHttpStatus(response.value().status_code);
  }

  lf::a2a::v1::AgentCard card;
  const auto parse = core::JsonToMessage(response.value().body, &card, {.ignore_unknown_fields = true});
  if (!parse.ok()) {
    return parse.error();
  }

  const auto validation = ValidateAgentCard(card);
  if (!validation.ok()) {
    return validation.error();
  }

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

core::Result<std::string> DiscoveryClient::BuildExtendedDiscoveryUrl(std::string_view base_url) {
  std::string normalized = Trim(base_url);
  if (normalized.empty()) {
    return core::Error::Validation("Base URL is required for extended Agent Card discovery");
  }
  if (!HasHttpScheme(normalized)) {
    return core::Error::Validation("Base URL must start with http:// or https://");
  }

  while (!normalized.empty() && normalized.back() == '/') {
    normalized.pop_back();
  }
  normalized.append(kExtendedAgentCardPath.data(), kExtendedAgentCardPath.size());
  return normalized;
}

core::Result<void> DiscoveryClient::ValidateAgentCard(const lf::a2a::v1::AgentCard& card) {
  if (card.supported_interfaces().empty()) {
    return core::Error::Validation("Agent Card must include at least one supported interface");
  }

  for (const auto& iface : card.supported_interfaces()) {
    if (iface.protocol_binding().empty()) {
      return core::Error::Validation("Agent Card contains an interface with unspecified protocol binding");
    }
    if (iface.protocol_version().empty()) {
      return core::Error::Validation("Agent Card contains an interface with no protocol version");
    }
    if (!core::Version::IsSupported(iface.protocol_version())) {
      return core::Error::UnsupportedVersion("Only A2A protocol version 1.0 is supported");
    }
    if (iface.url().empty()) {
      return core::Error::Validation("Agent Card contains an interface without a URL");
    }
    if (!IsValidInterfaceEndpoint({.protocol_binding = iface.protocol_binding(), .endpoint = iface.url()})) {
      return core::Error::Validation("Agent Card interface endpoint is invalid for its protocol binding");
    }
    for (const auto& requirement : card.security_requirements()) {
      for (const auto& [scheme_name, _] : requirement.schemes()) {
        if (!card.security_schemes().contains(scheme_name)) {
          return core::Error::Validation("Agent Card security requirement references an unknown security scheme: " +
                                         scheme_name);
        }
      }
    }
  }
  return {};
}

std::array<std::string_view, 3> BuildTransportOrder(std::string_view preferred_wire) {
  if (preferred_wire == kHttpJson) {
    return {kHttpJson, kJsonRpc, kGrpc};
  }
  if (preferred_wire == kJsonRpc) {
    return {kJsonRpc, kHttpJson, kGrpc};
  }
  return {kGrpc, kHttpJson, kJsonRpc};
}

void PopulateSecurityMetadata(const lf::a2a::v1::AgentCard& card, ResolvedInterface& resolved) {
  for (const auto& requirement : card.security_requirements()) {
    for (const auto& [scheme_name, _] : requirement.schemes()) {
      resolved.security_requirements.push_back(scheme_name);
    }
  }
  for (const auto& name : resolved.security_requirements) {
    const auto scheme = card.security_schemes().find(name);
    if (scheme != card.security_schemes().end()) {
      resolved.security_schemes.emplace(name, scheme->second);
    }
  }
}

core::Result<ResolvedInterface> AgentCardResolver::SelectPreferredInterface(const lf::a2a::v1::AgentCard& card,
                                                                            PreferredTransport preferred) {
  const auto preferred_wire = ToWireTransport(preferred);
  if (!preferred_wire.has_value()) {
    return core::Error::Validation("Invalid preferred transport requested");
  }

  for (const auto transport : BuildTransportOrder(preferred_wire.value())) {
    for (const auto& iface : card.supported_interfaces()) {
      if (iface.protocol_binding() != transport || !ValidateInterface(iface).ok()) {
        continue;
      }
      ResolvedInterface resolved;
      resolved.transport = ToPreferredTransport(iface.protocol_binding());
      resolved.url = iface.url();
      PopulateSecurityMetadata(card, resolved);
      return resolved;
    }
  }

  return core::Error::Validation("No usable interface found for this Agent Card");
}

core::Result<void> AgentCardResolver::ValidateInterface(const lf::a2a::v1::AgentInterface& iface) {
  if (iface.protocol_binding().empty()) {
    return core::Error::Validation("Unsupported protocol binding");
  }
  if (iface.url().empty()) {
    return core::Error::Validation("Missing interface URL");
  }
  if (!IsValidInterfaceEndpoint({.protocol_binding = iface.protocol_binding(), .endpoint = iface.url()})) {
    return core::Error::Validation("Interface endpoint is invalid for its protocol binding");
  }
  return {};
}

}  // namespace a2a::client
