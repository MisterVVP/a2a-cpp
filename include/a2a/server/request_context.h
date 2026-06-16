// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace a2a::server {

struct RequestContext final {
  std::optional<std::string> request_id;
  std::optional<std::string> remote_address;
  std::unordered_map<std::string, std::string> auth_metadata;
  std::unordered_map<std::string, std::string> client_headers;
};

[[nodiscard]] std::unordered_map<std::string, std::string> ExtractAuthMetadata(
    const std::unordered_map<std::string, std::string>& headers);

}  // namespace a2a::server
