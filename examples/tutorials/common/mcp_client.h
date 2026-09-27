#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "a2a/core/result.h"

namespace tutorial_mcp {

class Client final {
 public:
  Client(std::string endpoint, std::string bearer_token, std::chrono::milliseconds timeout);
  [[nodiscard]] a2a::core::Result<std::string> ReadResource(std::string_view uri) const;

 private:
  std::string endpoint_;
  std::string bearer_token_;
  std::chrono::milliseconds timeout_;
};

}  // namespace tutorial_mcp
