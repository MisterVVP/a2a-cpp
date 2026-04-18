#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "result.h"

namespace a2a::core {

class Extensions final {
 public:
  static constexpr std::string_view kHeaderName = "A2A-Extensions";

  [[nodiscard]] static std::string Format(const std::vector<std::string>& extensions);
  [[nodiscard]] static Result<std::vector<std::string>> Parse(std::string_view header_value);
};

}  // namespace a2a::core
