// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/json_value.h"

#include <simdjson.h>

#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>

namespace a2a::core::json {
namespace {

[[nodiscard]] bool IsJsonWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

[[nodiscard]] std::string_view TrimTrailingJsonWhitespace(std::string_view value) noexcept {
  while (!value.empty() && IsJsonWhitespace(value.back())) {
    value.remove_suffix(1U);
  }
  return value;
}

}  // namespace

std::optional<ValueRange> FindTopLevelObjectMemberValue(std::string_view json, std::string_view member_name) noexcept {
  try {
    const simdjson::padded_string padded(json);

    // On-Demand intentionally validates lazily. Preserve this API's historical
    // contract by validating the complete document before extracting a raw range.
    simdjson::dom::parser validation_parser;
    if (validation_parser.parse(padded).error() != simdjson::SUCCESS) {
      return std::nullopt;
    }

    simdjson::ondemand::parser parser;
    auto document = parser.iterate(padded);
    auto object = document.get_object();
    if (object.error() != simdjson::SUCCESS) {
      return std::nullopt;
    }

    std::optional<ValueRange> result;
    for (auto field : object.value_unsafe()) {
      auto key = field.key();
      if (key.error() != simdjson::SUCCESS) {
        return std::nullopt;
      }
      auto raw_json = field.value().raw_json();
      if (raw_json.error() != simdjson::SUCCESS) {
        return std::nullopt;
      }
      const char* const raw_key = key.value_unsafe().raw();
      const char* const key_contents = *raw_key == '"' ? raw_key + 1 : raw_key;
      const bool key_matches = std::memcmp(key_contents, member_name.data(), member_name.size()) == 0 &&
                               key_contents[member_name.size()] == '"';
      if (key_matches) {
        if (result.has_value()) {
          return std::nullopt;
        }
        const std::string_view raw = TrimTrailingJsonWhitespace(raw_json.value_unsafe());
        const auto offset = static_cast<std::size_t>(raw.data() - padded.data());
        result = ValueRange{.begin = offset, .end = offset + raw.size()};
      }
    }
    return document.error() == simdjson::SUCCESS ? result : std::nullopt;
  } catch (const simdjson::simdjson_error&) {
    return std::nullopt;
  }
}

}  // namespace a2a::core::json
