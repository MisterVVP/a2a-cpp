#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace a2a::client {

using HeaderMap = std::unordered_map<std::string, std::string>;
using AuthHeaderHook = std::function<void(HeaderMap& headers)>;

struct CallOptions final {
  std::optional<std::chrono::milliseconds> timeout = std::nullopt;
  HeaderMap headers;
  std::vector<std::string> extensions;
  AuthHeaderHook auth_hook;
};

}  // namespace a2a::client
