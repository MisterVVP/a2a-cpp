#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/result.h"
#include "google/protobuf/struct.pb.h"

namespace tutorial_mcp {

inline a2a::core::Result<void> ValidateResourceUri(std::string_view uri, std::string_view field_name) {
  constexpr std::string_view kRequiredMessage = " must be a non-empty string without control characters";
  const bool contains_control_character = std::ranges::any_of(uri, [](unsigned char character) {
    constexpr unsigned char kAsciiSpace = 0x20;
    constexpr unsigned char kAsciiDelete = 0x7f;
    return character < kAsciiSpace || character == kAsciiDelete;
  });
  if (!uri.empty() && !contains_control_character) {
    return {};
  }
  std::string message;
  message.reserve(field_name.size() + kRequiredMessage.size());
  message.append(field_name).append(kRequiredMessage);
  return a2a::core::Error::Validation(std::move(message));
}

inline a2a::core::Result<void> ValidateResourceUriField(const google::protobuf::Value& value,
                                                        std::string_view field_name) {
  if (value.kind_case() != google::protobuf::Value::kStringValue) {
    return ValidateResourceUri({}, field_name);
  }
  return ValidateResourceUri(value.string_value(), field_name);
}

}  // namespace tutorial_mcp
