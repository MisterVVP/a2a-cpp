// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>

#include "a2a/core/error.h"
#include "a2a/core/protocol_bindings.h"
#include "a2a/core/result.h"
#include "a2a/core/version.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::core {

class AgentCardBuilder final {
 public:
  AgentCardBuilder& SetName(std::string_view name);
  AgentCardBuilder& SetVersion(std::string_view version);
  AgentCardBuilder& SetDescription(std::string_view description);
  AgentCardBuilder& AddDefaultInputMode(std::string_view mode);
  AgentCardBuilder& AddDefaultOutputMode(std::string_view mode);
  AgentCardBuilder& WithPushNotifications(bool enabled);

  struct InterfaceSpec final {
    std::string_view binding;
    std::string_view version;
    std::string_view url;
  };

  AgentCardBuilder& AddInterface(const InterfaceSpec& spec);

  [[nodiscard]] Result<void> Validate() const;
  [[nodiscard]] lf::a2a::v1::AgentCard Build() const;

  [[nodiscard]] static AgentCardBuilder RestPreset(std::string_view name, std::string_view url,
                                                   std::string_view version = Version::kAgentCardVersion);
  [[nodiscard]] static AgentCardBuilder JsonRpcPreset(std::string_view name, std::string_view url,
                                                      std::string_view version = Version::kAgentCardVersion);
  [[nodiscard]] static AgentCardBuilder GrpcPreset(std::string_view name, std::string_view url,
                                                   std::string_view version = Version::kAgentCardVersion);
  struct ConformancePresetSpec final {
    std::string_view rest_url;
    std::string_view json_rpc_url;
    std::string_view grpc_url;
  };

  [[nodiscard]] static AgentCardBuilder ConformancePreset(const ConformancePresetSpec& spec,
                                                          std::string_view name = "Conformance SUT",
                                                          std::string_view version = Version::kAgentCardVersion,
                                                          std::string_view description = "A2A conformance agent");

 private:
  lf::a2a::v1::AgentCard card_;
};

}  // namespace a2a::core
