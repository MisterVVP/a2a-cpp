// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/agent_card_builder.h"

#include <string>
#include <string_view>
#include <unordered_set>

namespace a2a::core {
namespace {

using protocol_bindings::kGrpc;
using protocol_bindings::kHttpJson;
using protocol_bindings::kJsonRpc;

constexpr std::string_view kDefaultProtocolVersion = "1.0";
constexpr std::string_view kDefaultModeTextPlain = "text/plain";
constexpr std::string_view kRestDescription = "example rest agent";
constexpr std::string_view kJsonRpcDescription = "example json-rpc agent";
constexpr std::string_view kGrpcDescription = "example grpc agent";

bool HasHttpScheme(std::string_view url) { return url.starts_with("http://") || url.starts_with("https://"); }

bool HasGrpcScheme(std::string_view url) {
  return url.starts_with("grpc://") || url.starts_with("grpcs://") || url.starts_with("dns:///");
}

bool HasHostPortShape(std::string_view endpoint) { return endpoint.find(':') != std::string_view::npos; }

bool IsValidInterfaceEndpoint(std::string_view protocol_binding, std::string_view endpoint) {
  if (protocol_binding == kHttpJson || protocol_binding == kJsonRpc) {
    return HasHttpScheme(endpoint);
  }
  if (protocol_binding == kGrpc) {
    return HasGrpcScheme(endpoint) || HasHttpScheme(endpoint) || HasHostPortShape(endpoint);
  }
  return false;
}

}  // namespace

AgentCardBuilder& AgentCardBuilder::SetName(std::string_view name) {
  card_.set_name(std::string(name));
  return *this;
}

AgentCardBuilder& AgentCardBuilder::SetVersion(std::string_view version) {
  card_.set_version(std::string(version));
  return *this;
}

AgentCardBuilder& AgentCardBuilder::SetDescription(std::string_view description) {
  card_.set_description(std::string(description));
  return *this;
}

AgentCardBuilder& AgentCardBuilder::AddDefaultInputMode(std::string_view mode) {
  card_.add_default_input_modes(std::string(mode));
  return *this;
}

AgentCardBuilder& AgentCardBuilder::AddDefaultOutputMode(std::string_view mode) {
  card_.add_default_output_modes(std::string(mode));
  return *this;
}

AgentCardBuilder& AgentCardBuilder::AddInterface(std::string_view binding, std::string_view version, std::string_view url) {
  auto* iface = card_.add_supported_interfaces();
  iface->set_protocol_binding(std::string(binding));
  iface->set_protocol_version(std::string(version));
  iface->set_url(std::string(url));
  return *this;
}

Result<void> AgentCardBuilder::Validate() const {
  if (card_.name().empty()) {
    return Error::Validation("Agent card name is required");
  }
  if (card_.version().empty()) {
    return Error::Validation("Agent card version is required");
  }
  if (card_.description().empty()) {
    return Error::Validation("Agent card description is required");
  }
  if (card_.supported_interfaces().empty()) {
    return Error::Validation("Agent card must include at least one interface");
  }

  std::unordered_set<std::string> seen_interfaces;
  for (const auto& iface : card_.supported_interfaces()) {
    if (iface.protocol_binding().empty()) {
      return Error::Validation("Agent card interface protocol binding is required");
    }
    if (iface.protocol_version().empty()) {
      return Error::Validation("Agent card interface protocol version is required");
    }
    if (iface.url().empty()) {
      return Error::Validation("Agent card interface URL is required");
    }
    if (!IsValidInterfaceEndpoint(iface.protocol_binding(), iface.url())) {
      return Error::Validation("Agent card interface URL is invalid for its protocol binding");
    }
    const std::string key = iface.protocol_binding() + "|" + iface.protocol_version() + "|" + iface.url();
    if (!seen_interfaces.insert(key).second) {
      return Error::Validation("Agent card contains duplicate interfaces");
    }
  }

  return {};
}

lf::a2a::v1::AgentCard AgentCardBuilder::Build() const { return card_; }

AgentCardBuilder AgentCardBuilder::RestPreset(std::string_view name, std::string_view url, std::string_view version) {
  AgentCardBuilder builder;
  builder.SetName(name)
      .SetVersion(version)
      .SetDescription(kRestDescription)
      .AddDefaultInputMode(kDefaultModeTextPlain)
      .AddDefaultOutputMode(kDefaultModeTextPlain)
      .AddInterface(kHttpJson, kDefaultProtocolVersion, url);
  return builder;
}

AgentCardBuilder AgentCardBuilder::JsonRpcPreset(std::string_view name, std::string_view url, std::string_view version) {
  AgentCardBuilder builder;
  builder.SetName(name)
      .SetVersion(version)
      .SetDescription(kJsonRpcDescription)
      .AddDefaultInputMode(kDefaultModeTextPlain)
      .AddDefaultOutputMode(kDefaultModeTextPlain)
      .AddInterface(kJsonRpc, kDefaultProtocolVersion, url);
  return builder;
}

AgentCardBuilder AgentCardBuilder::GrpcPreset(std::string_view name, std::string_view url, std::string_view version) {
  AgentCardBuilder builder;
  builder.SetName(name)
      .SetVersion(version)
      .SetDescription(kGrpcDescription)
      .AddDefaultInputMode(kDefaultModeTextPlain)
      .AddDefaultOutputMode(kDefaultModeTextPlain)
      .AddInterface(kGrpc, kDefaultProtocolVersion, url);
  return builder;
}

}  // namespace a2a::core
