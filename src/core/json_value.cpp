// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Vladimir Pavlov <mistervvp@outlook.com> (https://github.com/MisterVVP)

#include "a2a/core/json_value.h"

#include <simdjson.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace a2a::core::json {

std::optional<ValueRange> FindTopLevelObjectMemberValue(std::string_view json, std::string_view member_name) noexcept {
  try {
    simdjson::ondemand::parser parser;
    const simdjson::padded_string padded(json);
    auto document = parser.iterate(padded);
    auto object = document.get_object();
    if (object.error()) {
      return std::nullopt;
    }

    std::optional<ValueRange> result;
    for (auto field : object.value_unsafe()) {
      auto key = field.key();
      if (key.error()) {
        return std::nullopt;
      }
      auto raw_json = field.value().raw_json();
      if (raw_json.error()) {
        return std::nullopt;
      }
      if (key.value_unsafe().raw() == member_name) {
        if (result.has_value()) {
          return std::nullopt;
        }
        const std::string_view raw = raw_json.value_unsafe();
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
