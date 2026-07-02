// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/agent_card/agent_card_serializer.h"

#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/legacy_transport_names.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/protojson.h"

namespace a2a::server {
namespace {

constexpr std::string_view kDefaultAgentCardVersion = "0.1.0";
constexpr std::string_view kDefaultTextMode = "text/plain";
constexpr std::string_view kVersionField = "version";
constexpr std::string_view kDescriptionField = "description";
constexpr std::string_view kCapabilitiesField = "capabilities";
constexpr std::string_view kStreamingField = "streaming";
constexpr std::string_view kPushNotificationsField = "pushNotifications";
constexpr std::string_view kDefaultInputModesField = "defaultInputModes";
constexpr std::string_view kDefaultOutputModesField = "defaultOutputModes";
constexpr std::string_view kSkillsField = "skills";
constexpr std::string_view kTagsField = "tags";
constexpr std::string_view kSupportedInterfacesField = "supportedInterfaces";
constexpr std::string_view kProtocolBindingField = "protocolBinding";

bool HasField(const google::protobuf::Struct& object, std::string_view key) {
  return object.fields().find(std::string(key)) != object.fields().end();
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

void EnsureStringField(google::protobuf::Struct* object, std::string_view key, std::string_view fallback) {
  if (!HasField(*object, key)) {
    (*object->mutable_fields())[std::string(key)].set_string_value(std::string(fallback));
  }
}

void EnsureBoolField(google::protobuf::Struct* object, std::string_view key, bool fallback) {
  if (!HasField(*object, key)) {
    (*object->mutable_fields())[std::string(key)].set_bool_value(fallback);
  }
}

void EnsureDefaultModeField(google::protobuf::Struct* card, std::string_view key) {
  if (HasField(*card, key)) {
    return;
  }
  auto* modes = EnsureListField(card, std::string(key))->mutable_list_value();
  modes->add_values()->set_string_value(std::string(kDefaultTextMode));
}

void EnsureSkillTags(google::protobuf::Struct* card) {
  auto* fields = card->mutable_fields();
  if (fields->find(std::string(kSkillsField)) == fields->end()) {
    EnsureListField(card, std::string(kSkillsField));
  }

  auto skills_it = fields->find(std::string(kSkillsField));
  if (skills_it == fields->end() || !skills_it->second.has_list_value()) {
    return;
  }

  for (auto& skill : *skills_it->second.mutable_list_value()->mutable_values()) {
    if (!skill.has_struct_value()) {
      continue;
    }
    EnsureListField(skill.mutable_struct_value(), std::string(kTagsField));
  }
}

void NormalizeAgentCardFields(google::protobuf::Struct* card) {
  EnsureStringField(card, kVersionField, kDefaultAgentCardVersion);
  EnsureStringField(card, kDescriptionField, "");

  auto* capabilities = EnsureStructField(card, std::string(kCapabilitiesField))->mutable_struct_value();
  EnsureBoolField(capabilities, kStreamingField, false);
  EnsureBoolField(capabilities, kPushNotificationsField, false);

  EnsureDefaultModeField(card, kDefaultInputModesField);
  EnsureDefaultModeField(card, kDefaultOutputModesField);
  EnsureSkillTags(card);
}

void AddLegacyTransportFields(google::protobuf::Struct* card, const lf::a2a::v1::AgentCard& agent_card) {
  if (card == nullptr) {
    return;
  }

  auto* fields = card->mutable_fields();
  auto interfaces_it = fields->find(std::string(kSupportedInterfacesField));
  if (interfaces_it != fields->end() && interfaces_it->second.has_list_value()) {
    for (auto& interface_value : *interfaces_it->second.mutable_list_value()->mutable_values()) {
      if (!interface_value.has_struct_value()) {
        continue;
      }
      auto* interface_fields = interface_value.mutable_struct_value()->mutable_fields();
      const auto binding_it = interface_fields->find(std::string(kProtocolBindingField));
      if (binding_it == interface_fields->end() ||
          binding_it->second.kind_case() != google::protobuf::Value::kStringValue) {
        continue;
      }
      (*interface_fields)[std::string(core::legacy_transport_names::kTransportField)].set_string_value(
          binding_it->second.string_value());
    }
  }

  if (fields->find(std::string(core::legacy_transport_names::kEndpointField)) != fields->end()) {
    return;
  }
  for (const auto& iface : agent_card.supported_interfaces()) {
    if (iface.protocol_binding() == core::protocol_bindings::kJsonRpc ||
        iface.protocol_binding() == core::protocol_bindings::kHttpJson) {
      (*fields)[std::string(core::legacy_transport_names::kEndpointField)].set_string_value(iface.url());
      (*fields)[std::string(core::legacy_transport_names::kPreferredTransportField)].set_string_value(
          iface.protocol_binding());
      break;
    }
  }
}

}  // namespace

core::Result<google::protobuf::Struct> BuildNormalizedAgentCard(const lf::a2a::v1::AgentCard& agent_card,
                                                                bool include_legacy_transport_fields) {
  const auto body = core::MessageToJson(agent_card);
  if (!body.ok()) {
    return body.error();
  }

  google::protobuf::Struct card;
  const auto parsed = core::JsonToMessage(body.value(), &card, {.ignore_unknown_fields = false});
  if (!parsed.ok()) {
    return parsed.error();
  }

  NormalizeAgentCardFields(&card);
  if (include_legacy_transport_fields) {
    AddLegacyTransportFields(&card, agent_card);
  }
  return card;
}

core::Result<google::protobuf::Value> BuildAgentCardJsonValue(const lf::a2a::v1::AgentCard& agent_card,
                                                              bool include_legacy_transport_fields) {
  const auto card = BuildNormalizedAgentCard(agent_card, include_legacy_transport_fields);
  if (!card.ok()) {
    return card.error();
  }

  google::protobuf::Value value;
  *value.mutable_struct_value() = card.value();
  return value;
}

}  // namespace a2a::server
