// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/server/required_extensions_validator.h"

#include <algorithm>
#include <utility>

#include "a2a/core/extensions.h"
#include "a2a/core/http_utils.h"
#include "a2a/core/protocol_errors.h"

namespace a2a::server {

RequiredExtensionsValidator::RequiredExtensionsValidator(std::vector<std::string> required_extensions)
    : required_extensions_(std::move(required_extensions)) {}

core::Result<void> RequiredExtensionsValidator::Validate(
    const std::unordered_map<std::string, std::string>& headers) const {
  if (required_extensions_.empty()) {
    return {};
  }

  const auto header = core::http::FindHeaderValue(headers, core::Extensions::kHeaderName);
  if (!header.has_value()) {
    return core::protocol_errors::ExtensionSupportRequired(std::string(kMissingRequiredExtensionMessage));
  }

  const auto parsed = core::Extensions::Parse(*header);
  if (!parsed.ok()) {
    return parsed.error();
  }

  for (const auto& required_extension : required_extensions_) {
    if (std::ranges::find(parsed.value(), required_extension) == parsed.value().end()) {
      return core::protocol_errors::ExtensionSupportRequired(std::string(kMissingRequiredExtensionMessage));
    }
  }

  return {};
}

}  // namespace a2a::server
