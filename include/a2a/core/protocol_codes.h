#pragma once

#include <string_view>

namespace a2a::core::protocol_codes {

inline constexpr std::string_view kTaskNotFound = "-32001";
inline constexpr std::string_view kTaskNotCancelable = "-32002";
inline constexpr std::string_view kUnsupportedOperation = "-32004";

}  // namespace a2a::core::protocol_codes