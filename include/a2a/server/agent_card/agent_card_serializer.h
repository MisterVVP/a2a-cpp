// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <google/protobuf/struct.pb.h>

#include "a2a/core/result.h"
#include "a2a/v1/a2a.pb.h"

namespace a2a::server {

[[nodiscard]] core::Result<google::protobuf::Struct> BuildNormalizedAgentCard(const lf::a2a::v1::AgentCard& agent_card,
                                                                              bool include_legacy_transport_fields);

[[nodiscard]] core::Result<google::protobuf::Value> BuildAgentCardJsonValue(const lf::a2a::v1::AgentCard& agent_card,
                                                                            bool include_legacy_transport_fields);

}  // namespace a2a::server
