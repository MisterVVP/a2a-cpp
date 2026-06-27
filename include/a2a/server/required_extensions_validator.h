// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "a2a/core/result.h"

namespace a2a::server {

class RequiredExtensionsValidator final {
 public:
  static constexpr std::string_view kMissingRequiredExtensionMessage = "Missing required A2A extension support";

  RequiredExtensionsValidator() = default;
  explicit RequiredExtensionsValidator(std::vector<std::string> required_extensions);

  [[nodiscard]] core::Result<std::vector<std::string>> Validate(
      const std::unordered_map<std::string, std::string>& headers) const;

  [[nodiscard]] const std::vector<std::string>& required_extensions() const noexcept { return required_extensions_; }

 private:
  std::vector<std::string> required_extensions_;
};

}  // namespace a2a::server
