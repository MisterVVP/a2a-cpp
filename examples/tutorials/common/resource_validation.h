#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/result.h"
#include "google/protobuf/struct.pb.h"

namespace tutorial_mcp {
namespace resource_validation_detail {
inline constexpr std::string_view kRequiredMessage = " must be a non-empty string without control characters";
}  // namespace resource_validation_detail

inline a2a::core::Result<void> ValidateResourceUri(std::string_view uri, std::string_view field_name) {
  const bool contains_control_character = std::ranges::any_of(uri, [](unsigned char character) {
    constexpr unsigned char kAsciiSpace = 0x20;
    constexpr unsigned char kAsciiDelete = 0x7f;
    return character < kAsciiSpace || character == kAsciiDelete;
  });
  if (!uri.empty() && !contains_control_character) {
    return {};
  }
  std::string message;
  message.reserve(field_name.size() + resource_validation_detail::kRequiredMessage.size());
  message.append(field_name).append(resource_validation_detail::kRequiredMessage);
  return a2a::core::Error::Validation(std::move(message));
}

inline a2a::core::Result<void> ValidateResourceUriField(const google::protobuf::Value& value,
                                                        std::string_view field_name) {
  if (value.kind_case() == google::protobuf::Value::kStringValue) {
    return ValidateResourceUri(value.string_value(), field_name);
  }
  std::string message;
  message.reserve(field_name.size() + resource_validation_detail::kRequiredMessage.size());
  message.append(field_name).append(resource_validation_detail::kRequiredMessage);
  return a2a::core::Error::Validation(std::move(message));
}

}  // namespace tutorial_mcp
