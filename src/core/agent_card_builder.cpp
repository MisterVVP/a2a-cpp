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
constexpr std::string_view kConformanceSkillId = "echo";
constexpr std::string_view kConformanceSkillName = "Echo Skill";
constexpr std::string_view kConformanceSkillDescription = "Echoes incoming text for conformance validation";
constexpr std::string_view kConformanceTag = "conformance";

bool HasHttpScheme(std::string_view url) { return url.starts_with("http://") || url.starts_with("https://"); }

bool HasGrpcScheme(std::string_view url) {
  return url.starts_with("grpc://") || url.starts_with("grpcs://") || url.starts_with("dns:///");
}

bool HasHostPortShape(std::string_view endpoint) { return endpoint.find(':') != std::string_view::npos; }

bool IsValidInterfaceEndpoint(const AgentCardBuilder::InterfaceSpec& spec) {
  if (spec.binding == kHttpJson || spec.binding == kJsonRpc) {
    return HasHttpScheme(spec.url);
  }
  if (spec.binding == kGrpc) {
    return HasGrpcScheme(spec.url) || HasHttpScheme(spec.url) || HasHostPortShape(spec.url);
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

AgentCardBuilder& AgentCardBuilder::AddInterface(const InterfaceSpec& spec) {
  auto* iface = card_.add_supported_interfaces();
  iface->set_protocol_binding(std::string(spec.binding));
  iface->set_protocol_version(std::string(spec.version));
  iface->set_url(std::string(spec.url));
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
    if (!IsValidInterfaceEndpoint(
            {.binding = iface.protocol_binding(), .version = iface.protocol_version(), .url = iface.url()})) {
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
      .AddInterface({.binding = kHttpJson, .version = kDefaultProtocolVersion, .url = url});
  return builder;
}

AgentCardBuilder AgentCardBuilder::JsonRpcPreset(std::string_view name, std::string_view url,
                                                 std::string_view version) {
  AgentCardBuilder builder;
  builder.SetName(name)
      .SetVersion(version)
      .SetDescription(kJsonRpcDescription)
      .AddDefaultInputMode(kDefaultModeTextPlain)
      .AddDefaultOutputMode(kDefaultModeTextPlain)
      .AddInterface({.binding = kJsonRpc, .version = kDefaultProtocolVersion, .url = url});
  return builder;
}

AgentCardBuilder AgentCardBuilder::GrpcPreset(std::string_view name, std::string_view url, std::string_view version) {
  AgentCardBuilder builder;
  builder.SetName(name)
      .SetVersion(version)
      .SetDescription(kGrpcDescription)
      .AddDefaultInputMode(kDefaultModeTextPlain)
      .AddDefaultOutputMode(kDefaultModeTextPlain)
      .AddInterface({.binding = kGrpc, .version = kDefaultProtocolVersion, .url = url});
  return builder;
}

AgentCardBuilder AgentCardBuilder::ConformancePreset(const ConformancePresetSpec& spec, std::string_view name,
                                                     std::string_view version, std::string_view description) {
  AgentCardBuilder builder;
  auto card = builder.SetName(name)
                  .SetVersion(version)
                  .SetDescription(description)
                  .AddDefaultInputMode(kDefaultModeTextPlain)
                  .AddDefaultOutputMode(kDefaultModeTextPlain)
                  .AddInterface({.binding = kJsonRpc, .version = kDefaultProtocolVersion, .url = spec.json_rpc_url})
                  .AddInterface({.binding = kHttpJson, .version = kDefaultProtocolVersion, .url = spec.rest_url})
                  .AddInterface({.binding = kGrpc, .version = kDefaultProtocolVersion, .url = spec.grpc_url})
                  .Build();
  auto* capabilities = card.mutable_capabilities();
  capabilities->set_streaming(true);
  capabilities->set_push_notifications(false);

  auto* skill = card.add_skills();
  skill->set_id(std::string(kConformanceSkillId));
  skill->set_name(std::string(kConformanceSkillName));
  skill->set_description(std::string(kConformanceSkillDescription));
  skill->add_input_modes(std::string(kDefaultModeTextPlain));
  skill->add_output_modes(std::string(kDefaultModeTextPlain));
  skill->add_tags(std::string(kConformanceTag));

  builder.card_ = std::move(card);
  return builder;
}

}  // namespace a2a::core
