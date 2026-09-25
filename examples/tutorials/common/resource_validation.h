#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "a2a/core/error.h"
#include "a2a/core/result.h"
#include "google/protobuf/struct.pb.h"

namespace tutorial_mcp {

inline a2a::core::Result<void> ValidateResourceUriField(const google::protobuf::Value& value,
                                                        std::string_view field_name) {
  if (value.kind_case() == google::protobuf::Value::kStringValue && !value.string_value().empty()) {
    return {};
  }
  std::string message;
  message.reserve(field_name.size() + std::string_view(" must be a non-empty string").size());
  message.append(field_name).append(" must be a non-empty string");
  return a2a::core::Error::Validation(std::move(message));
}

}  // namespace tutorial_mcp
