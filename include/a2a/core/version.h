#pragma once

#include <string>
#include <string_view>

namespace a2a::core {

class Version final {
 public:
  static constexpr std::string_view kProtocolVersion = "1.0";
  static constexpr std::string_view kHeaderName = "A2A-Version";

  [[nodiscard]] static std::string HeaderValue();
  [[nodiscard]] static bool IsSupported(std::string_view version) noexcept;
};

}  // namespace a2a::core
