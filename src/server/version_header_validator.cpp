// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/version_header_validator.h"

#include <string>

#include "a2a/core/protocol_errors.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/version.h"

namespace a2a::server {

VersionHeaderValidator::VersionHeaderValidator(bool require_version_header) noexcept
    : require_version_header_(require_version_header) {}

core::Result<void> VersionHeaderValidator::Validate(const std::unordered_map<std::string, std::string>& headers) const {
  const auto version_header = core::http::FindHeaderValue(headers, core::Version::kHeaderName);
  if (!version_header.has_value() || version_header->empty()) {
    if (require_version_header_) {
      return core::protocol_errors::VersionNotSupported(std::string(kMissingRequiredVersionMessage));
    }
    return {};
  }

  if (!core::Version::IsSupported(*version_header)) {
    return core::protocol_errors::VersionNotSupported(std::string(kUnsupportedVersionMessage));
  }

  return {};
}

}  // namespace a2a::server
