// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace a2a::core {

class Version final {
 public:
  static constexpr std::string_view kProtocolVersion = "1.0";
  static constexpr std::string_view kAgentCardVersion = "1.0.0";
  static constexpr std::string_view kHeaderName = "A2A-Version";
  static constexpr std::string_view kEmptyVersionFallback = "0.3";

  [[nodiscard]] static std::string HeaderValue();
  [[nodiscard]] static std::optional<std::string_view> MajorMinor(std::string_view version) noexcept;
  [[nodiscard]] static bool IsSupported(std::string_view version) noexcept;
};

}  // namespace a2a::core
