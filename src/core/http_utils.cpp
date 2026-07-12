// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/http_utils.h"

#include "a2a/core/http_constants.h"
#include "a2a/core/string_utils.h"

namespace a2a::core::http {

std::optional<std::string_view> FindHeaderValue(const std::unordered_map<std::string, std::string>& headers,
                                                std::string_view name) {
  for (const auto& [header_name, value] : headers) {
    if (strings::EqualsAsciiCaseInsensitive(header_name, name)) {
      return std::string_view(value);
    }
  }
  return std::nullopt;
}

bool IsMediaType(std::string_view content_type, std::string_view expected_media_type) {
  const std::size_t parameter_start = content_type.find(kContentTypeParameterSeparator);
  const std::string_view media_type = strings::TrimAsciiWhitespace(content_type.substr(0, parameter_start));
  return strings::EqualsAsciiCaseInsensitive(media_type, expected_media_type);
}

bool IsJsonContentType(std::string_view content_type) { return IsMediaType(content_type, kContentTypeApplicationJson); }

bool IsSseContentType(std::string_view content_type) { return IsMediaType(content_type, kContentTypeTextEventStream); }

}  // namespace a2a::core::http
