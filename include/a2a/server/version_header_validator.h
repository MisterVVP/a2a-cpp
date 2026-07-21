// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "a2a/core/result.h"

namespace a2a::server {

class VersionHeaderValidator final {
 public:
  static constexpr std::string_view kMissingRequiredVersionMessage = "Missing required A2A-Version header";
  static constexpr std::string_view kUnsupportedVersionMessage = "Unsupported A2A-Version header value";

  VersionHeaderValidator() = default;
  explicit VersionHeaderValidator(bool require_version_header) noexcept;

  [[nodiscard]] core::Result<void> Validate(const std::unordered_map<std::string, std::string>& headers) const;

  [[nodiscard]] bool require_version_header() const noexcept { return require_version_header_; }

 private:
  bool require_version_header_ = true;
};

}  // namespace a2a::server
