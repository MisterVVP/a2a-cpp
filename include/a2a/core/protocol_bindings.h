#pragma once

#include <string_view>

namespace a2a::core::protocol_bindings {

inline constexpr std::string_view kHttpJson = "HTTP+JSON";
inline constexpr std::string_view kJsonRpc = "JSONRPC";
inline constexpr std::string_view kGrpc = "GRPC";

}  // namespace a2a::core::protocol_bindings

