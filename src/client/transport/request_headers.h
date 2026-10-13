// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <unordered_map>

#include "a2a/client/call_options.h"
#include "a2a/core/result.h"
#include "a2a/core/string_utils.h"
#include "a2a/core/version.h"

namespace a2a::client::internal {

inline constexpr std::string_view kInvalidProtocolVersion =
    "A2A protocol version must be Major.Minor or Major.Minor.Patch";

// Apply after credentials/hooks so every request has exactly one version,
// including when user headers contain differently cased aliases.
[[nodiscard]] inline core::Result<void> ApplyProtocolVersion(const CallOptions& options, HeaderMap& headers) {
  const auto version = core::Version::MajorMinor(options.protocol_version);
  if (!version.has_value()) {
    return core::Error::Validation(std::string(kInvalidProtocolVersion));
  }
  std::erase_if(headers, [](const auto& header) {
    return core::strings::EqualsAsciiCaseInsensitive(header.first, core::Version::kHeaderName);
  });
  headers[std::string(core::Version::kHeaderName)] =
      options.protocol_version.empty() ? std::string{} : std::string(*version);
  return {};
}

}  // namespace a2a::client::internal
